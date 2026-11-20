#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual shared C parser/UI in the official ESP-GSP 1.5.1 sim.

Run after building the native backend, for example:
  python -I pc/tests/test_ui_integration.py --project C:/path/to/bambu_monitor \
      --sim C:/path/to/1.5.1/sim.exe --output C:/path/to/new-evidence-directory

No device/network printer transport is linked. Input uses the documented
Content-Length JSON-RPC API; backend-only application state writes are never
used. Host-only snapshots capture actual UI setter inputs and callback effects.
All subprocesses and temporary listeners are owned by this invocation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import traceback
from typing import Callable


class Rpc:
    """Sequential framed client, retaining notifications between replies."""
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=12)
        self.socket.settimeout(12)
        self.reader = self.socket.makefile("rb")
        self.sequence = 0
        self.notifications: list[dict] = []

    def call(self, method: str, params: dict | None = None):
        self.sequence += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self.sequence,
                           "method": method, "params": params or {}}).encode()
        self.socket.sendall(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        while True:
            length = None
            while True:
                line = self.reader.readline()
                if not line:
                    raise EOFError(f"simulator closed RPC during {method}")
                if line == b"\r\n":
                    break
                key, value = line.decode("ascii").split(":", 1)
                if key.lower() == "content-length":
                    length = int(value)
            if length is None or not 0 < length <= 16 * 1024 * 1024:
                raise ValueError(f"invalid RPC frame length: {length}")
            payload = self.reader.read(length)
            if len(payload) != length:
                raise EOFError(f"truncated RPC reply during {method}")
            reply = json.loads(payload)
            if "method" in reply:
                self.notifications.append(reply)
                continue
            if reply.get("id") != self.sequence:
                raise AssertionError(f"unexpected RPC response: {reply}")
            if "error" in reply:
                raise AssertionError(f"{method} failed: {reply['error']}")
            result = reply.get("result")
            if isinstance(result, dict) and result.get("result_code", 0) != 0:
                raise AssertionError(f"{method} result failure: {result}")
            return result

    def close(self):
        self.reader.close()
        self.socket.close()


def atomic_text(path: Path, text: str):
    temporary = path.with_suffix(path.suffix + ".next")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def records(path: Path) -> list[dict]:
    if not path.exists():
        return []
    # An appended line may still be in flight; only consume complete records.
    data = path.read_bytes().split(b"\n")[:-1]
    return [json.loads(line) for line in data if line]


def free_ports() -> tuple[int, int]:
    with socket.socket() as first, socket.socket() as second:
        first.bind(("127.0.0.1", 0))
        second.bind(("127.0.0.1", 0))
        return first.getsockname()[1], second.getsockname()[1]


class Session:
    def __init__(self, args, name: str, initial: dict):
        self.args = args
        self.name = name
        self.directory = args.output / name
        self.directory.mkdir()
        self.observations = self.directory / "observations.jsonl"
        self.transport = self.directory / "transport.jsonl"
        self.report_file = self.directory / "report.json"
        self.clock_file = self.directory / "clock.txt"
        self.ready_file = self.directory / "ready.json"
        self.event_file = self.directory / "bmb_evt.txt"
        self.processes: list[subprocess.Popen] = []
        self.logs = []
        self.api: Rpc | None = None
        self.now = 0
        self.report_sequence = 0
        self.checks: list[dict] = []
        self.screenshots: list[str] = []
        self.latest: dict = {}
        atomic_text(self.clock_file, "0")
        atomic_text(self.report_file, json.dumps({"print": initial}))
        self.sidecar = json.loads(args.sidecar.read_text(encoding="utf-8"))
        scene = self.sidecar["scenes"][0]
        self.callbacks = {item["name"]: item["action_id"] for item in scene["callbacks"]}
        self.objects = {item["name"] for item in scene["objects"]}

    def launch(self, executable: Path, arguments: list[str], name: str, env=None):
        log = (self.directory / f"{name}.log").open("wb")
        self.logs.append(log)
        process = subprocess.Popen([str(executable), *arguments], cwd=self.directory,
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        self.processes.append(process)
        return process

    def healthy(self):
        for process in self.processes:
            if process.poll() is not None:
                raise AssertionError(f"owned process {process.pid} exited {process.returncode}; "
                                     f"see {self.directory}")

    def wait_for(self, predicate: Callable[[], object], description: str, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.healthy()
            value = predicate()
            if value:
                return value
            time.sleep(0.04)
        raise AssertionError(f"timeout: {description}; latest UI={self.latest}; "
                             f"evidence={self.directory}")

    def start(self):
        backend_port, api_port = free_ports()
        endpoint = f"tcp://127.0.0.1:{backend_port}"
        simulator = self.launch(self.args.sim, [
            "--headless", "--backend-listen", endpoint, "--backend-required",
            "--api-listen", f"tcp://127.0.0.1:{api_port}",
            "--api-json", str(self.args.sidecar), "--bundle", str(self.args.bundle),
            *(["--dynamic-font", str(self.args.font)] if self.args.dynamic_font else []),
            "--ready-file", str(self.ready_file),
            "--input-mode", "api-exclusive"], "simulator")

        def ready():
            try:
                value = json.loads(self.ready_file.read_text())
            except (OSError, ValueError):
                return None
            if value.get("pid") != simulator.pid:
                raise AssertionError(f"ready-file PID is not owned: {value}")
            # The ready-file version is its schema, not the package version.
            if value.get("version") != 1 or value.get("bridge_version") != 1:
                raise AssertionError(f"unsupported simulator ready schema: {value}")
            if value.get("backend") != endpoint:
                raise AssertionError(f"unexpected backend endpoint: {value}")
            return value

        self.ready = self.wait_for(ready, "fresh simulator ready file")
        self.api = Rpc(api_port)
        capabilities = self.api.call("capabilities")
        if not capabilities.get("named_api"):
            raise AssertionError(f"named API sidecar unavailable: {capabilities}")
        self.api.call("subscribe", {"events": ["callback"]})
        env = os.environ.copy()
        env.update(BMB_SIM_REPORT_FILE=str(self.report_file),
                   BMB_SIM_CLOCK_FILE=str(self.clock_file),
                   BMB_SIM_TRANSPORT_FILE=str(self.transport),
                   BMB_SIM_OBSERVE_FILE=str(self.observations),
                   BMB_SIM_EPOCH="1800000000", TZ="UTC0")
        self.launch(self.args.backend, ["--endpoint", endpoint], "backend", env)
        self.observe(lambda value: value.get("link") == 2, "initial report rendered")
        return self

    def observe(self, predicate: Callable[[dict], bool], description: str, after=-1):
        def poll():
            values = records(self.observations)
            if values:
                self.latest = values[-1]
                if self.latest.get("last_error") != 0:
                    raise AssertionError(f"C UI setter/runtime error: {self.latest}")
                if self.latest["tick"] > after and predicate(self.latest):
                    return self.latest
            return None
        return self.wait_for(poll, description)

    def check(self, name: str, predicate: Callable[[dict], bool], after=-1):
        value = self.observe(predicate, name, after)
        self.checks.append({"name": name, "status": "pass", "observation": value})
        return value

    def deliver(self, payload: dict, advance=0):
        previous = sum(item["event"] == "report" for item in records(self.transport))
        tick = self.latest.get("tick", -1)
        self.now += advance
        atomic_text(self.clock_file, str(self.now))
        atomic_text(self.report_file, json.dumps(payload, ensure_ascii=False))
        self.wait_for(lambda: sum(item["event"] == "report" for item in
                                 records(self.transport)) > previous,
                      "shared parser consumed fixture")
        snapshots = records(self.observations)
        if snapshots:
            tick = max(tick, snapshots[-1]["tick"])
        return self.observe(lambda value: value["now_ms"] == self.now,
                            "fixture reflected by shared UI refresh", after=tick)

    def send(self, fields: dict, advance=0):
        self.report_sequence += 1
        # A sequence field makes repeated telemetry a distinct delivery through
        # the fixture route, rather than a no-op on identical file contents.
        payload = {"print": {"sequence_id": str(self.report_sequence), **fields}}
        value = self.deliver(payload, advance)
        if value["last_report_ms"] != self.now:
            raise AssertionError(f"telemetry timestamp did not advance: {value}")
        return value

    def advance(self, milliseconds: int):
        self.now += milliseconds
        atomic_text(self.clock_file, str(self.now))
        return self.observe(lambda value: value["now_ms"] == self.now,
                            "controlled monotonic clock advanced")

    def tap(self, object_name: str, callback: str):
        if object_name not in self.objects:
            raise AssertionError(f"missing real scene object {object_name}")
        action = self.callbacks[callback]
        before = len(self.event_file.read_text().splitlines()) if self.event_file.exists() else 0
        notification_start = len(self.api.notifications)
        self.api.call("tap_object", {"name": object_name})
        # Frames are a transport aid; actual C callback and state are asserted.
        self.api.call("wait", {"frames": 3})
        def callback_received():
            lines = self.event_file.read_text().splitlines() if self.event_file.exists() else []
            backend_received = f"action={action}" in lines[before:]
            api_received = any(item.get("method") == "callback" and
                               item.get("params", {}).get("action_id") == action
                               for item in self.api.notifications[notification_start:])
            return backend_received and api_received
        self.wait_for(callback_received, f"actual backend callback {callback}")
        # The callback trace is written on entry, before synchronous page
        # visibility RPCs finish. Wait for a later complete C refresh before
        # tapping the destination page, rather than racing its visibility.
        snapshots = records(self.observations)
        after = snapshots[-1]["tick"] if snapshots else -1
        self.observe(lambda value: True, f"C refresh after {callback}", after=after)
        self.checks.append({"name": f"callback:{callback}", "status": "pass",
                            "object": object_name, "action_id": action})

    def screenshot(self, name: str):
        self.api.call("wait", {"frames": 3})
        path = self.directory / f"{name}.png"
        self.api.call("screenshot", {"path": str(path), "format": "png"})
        if not path.exists() or path.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
            raise AssertionError(f"simulator did not save a valid PNG: {path}")
        self.screenshots.append(str(path))

    def quiet(self, description: str, ticks=4):
        before = self.latest["tick"]
        self.check(description, lambda value: value["alert_kind"] == 0 and
                   value["page"] == 0 and not value["error_seen"] and
                   not value["finish_seen"], after=before + ticks)
        recent = [value for value in records(self.observations) if value["tick"] > before]
        if any(value["alert_kind"] or value["page"] == 3 for value in recent):
            raise AssertionError(f"transient unexpected alert: {recent}")

    def close(self):
        if self.api:
            try:
                self.api.call("quit")
            except (OSError, EOFError, AssertionError):
                pass
            self.api.close()
        for process in reversed(self.processes):
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)
        for log in self.logs:
            log.close()
        self.ready_file.unlink(missing_ok=True)

    def result(self, error=None):
        return {"name": self.name, "status": "fail" if error else "pass",
                "error": error, "checks": self.checks, "latest": self.latest,
                "screenshots": self.screenshots, "evidence": str(self.directory),
                "owned_pids": [process.pid for process in self.processes]}


def initial_snapshot(session: Session, state: str):
    session.check(f"initial {state} snapshot quiet even at monotonic zero",
                  lambda value: value["now_ms"] == 0 and value["alert_kind"] == 0 and
                  value["page"] == 0 and not value["finish_seen"] and
                  not value["error_seen"])
    session.send({"gcode_state": state, "print_error": 0xFFFFFFFF if state == "ERROR" else 0})
    session.quiet(f"repeated initial {state} remains quiet")
    session.screenshot("initial-quiet")
    if state == "ERROR":
        session.send({"gcode_state": "RUNNING", "print_error": 0,
                      "subtask_name": "early-current-task.3mf"})
        session.send({"gcode_state": "ERROR", "print_error": 0xFFFFFFFF})
        session.check("later live error at ms zero has no startup grace delay", lambda value:
                      value["now_ms"] == 0 and value["alert_kind"] == 2 and
                      value["page"] == 3 and
                      value["text"].get("finish_task") == "early-current-task.3mf" and
                      value["text"].get("finish_sub") == "错误码 0xFFFFFFFF")
        session.screenshot("early-live-error")


def live_feature_batch(s: Session):
    s.send({"gcode_state": "RUNNING", "subtask_name": "current-bracket.3mf",
            "mc_percent": 91, "layer_num": 210, "total_layers": 210,
            "mc_remaining_time": 83, "print_error": 0,
            "model_id": "CNf0baae0571025", "nozzle_temper": 220,
            "bed_temper": 60, "lights_report": [{"node": "chamber_light", "mode": "off"}],
            "ams": {"ams": [{"id": "0", "humidity": "4", "temp": "25.6",
                              "tray": [{"id": "0", "remain": 72, "tray_color": "F97316FF", "tray_type": "PLA"}]},
                             {"id": "1", "tray": [{"id": "0", "remain": 44,
                              "tray_color": "00AAFFFF", "tray_type": "PETG"}]}]}}, advance=1000)
    running = s.check("RUNNING renders ETA and contextual pause/light labels",
                      lambda value: value["print_state"] == 3 and value["page"] == 0 and
                      "剩余 1h23m" in value["text"].get("layer_lbl", "") and
                      "预计 " in value["text"]["layer_lbl"] and
                      value["text"].get("qk_pause_btn_lbl") == "暂停" and
                      value["text"].get("qk_light_btn_lbl") == "灯 关")
    initial_eta = running["text"]["layer_lbl"].split("预计 ")[1]
    remaining_timestamp = running["remaining_updated_ms"]
    s.screenshot("running-four-controls-layer210")
    s.tap("qk_pause_btn", "qk_pause")
    s.check("quick pause updates real model and hides ETA", lambda value:
            value["print_state"] == 4 and not value["ctrl_busy"] and
            value["text"].get("qk_pause_btn_lbl") == "继续" and
            "预计" not in value["text"].get("layer_lbl", ""))
    s.screenshot("paused")
    s.tap("qk_pause_btn", "qk_pause")
    s.check("quick resume updates real model", lambda value:
            value["print_state"] == 3 and not value["ctrl_busy"] and
            value["text"].get("qk_pause_btn_lbl") == "暂停")
    s.tap("qk_light_btn", "qk_light")
    s.check("quick light waits for two matched ACKs then updates label", lambda value:
            value["light_on"] == 1 and not value["ctrl_busy"] and
            value["command_parts"] == 2 and value["command_ack_mask"] == 3 and
            value["text"].get("qk_light_btn_lbl") == "灯 开" and
            value["text"].get("prog_hint") == "打印机已确认")
    partial = [value for value in records(s.observations)
               if value["command_kind"] == 4 and value["command_ack_mask"] == 1]
    if not partial or any(not value["ctrl_busy"] for value in partial):
        raise AssertionError("LIGHT was not pending between its two matched ACKs")
    s.checks.append({"name": "first LIGHT ACK keeps request pending", "status": "pass"})
    s.tap("qk_ams_btn", "ams_open")
    s.check("AMS opens with both parsed units", lambda value:
            value["page"] == 5 and value["text"].get("ams_remain_lbl_0") == "72%" and
            value["text"].get("ams_remain_lbl_4") == "44%" and
            value["text"].get("ams_type_lbl_4") == "PETG")
    s.screenshot("ams-two-units")
    s.tap("ams_back_btn", "ams_back")
    s.check("AMS back returns to status", lambda value: value["page"] == 0)
    s.tap("qk_ctrl_btn", "ctrl_open")
    s.check("fourth running-bar control opens Controls", lambda value: value["page"] == 4)
    s.screenshot("controls")
    s.tap("ctrl_spd_btn_3", "ctrl_spd3")
    s.check("speed waits for one matched ACK", lambda value:
            value["spd_lvl"] == 3 and not value["ctrl_busy"] and
            value["command_kind"] == 3 and value["command_parts"] == 1 and
            value["command_ack_mask"] == 1 and
            value["text"].get("ctrl_status_lbl") == "打印机已确认")
    s.tap("ctrl_stop_btn", "ctrl_stop")
    s.check("first stop tap arms without command", lambda value:
            value["stop_armed"] and value["text"].get("ctrl_stop_btn_lbl") == "再按一次确认停止")
    s.tap("ctrl_back_btn", "ctrl_back")
    s.check("navigation cancels armed stop", lambda value:
            value["page"] == 0 and not value["stop_armed"] and
            value["text"].get("ctrl_stop_btn_lbl") == "停止打印")
    s.tap("qk_ctrl_btn", "ctrl_open")
    s.tap("ctrl_stop_btn", "ctrl_stop")
    s.check("stop reentry requires a fresh first tap", lambda value:
            value["page"] == 4 and value["stop_armed"] and not value["ctrl_busy"])
    s.tap("ctrl_back_btn", "ctrl_back")
    commands = [entry["value"] for entry in records(s.transport) if entry["event"] == "command"]
    if commands != [0, 1]:
        raise AssertionError(f"unexpected host printer-command sequence: {commands}")
    s.checks.append({"name": "pause/resume only; armed stop never sent", "status": "pass", "commands": commands})

    s.send({"gcode_state": "RUNNING", "mc_percent": 92}, advance=60000)
    s.check("unrelated telemetry never resets ETA or drifts finish clock", lambda value:
            value["remaining_updated_ms"] == remaining_timestamp and
            "剩余 1h22m" in value["text"].get("layer_lbl", "") and
            value["text"]["layer_lbl"].endswith("预计 " + initial_eta))
    last_telemetry = s.latest["last_report_ms"]
    s.advance(15000)
    s.check("shared stale watchdog downgrades link without rewriting telemetry time", lambda value:
            value["link"] == 1 and value["last_report_ms"] == last_telemetry and
            "预计" not in value["text"].get("layer_lbl", "") and
            "剩余" not in value["text"].get("layer_lbl", ""))
    s.deliver({"system": {"command": "ledctrl", "sequence_id": "999999",
                          "result": "SUCCESS"}}, advance=1000)
    s.check("ACK after stale watchdog cannot revive link, timestamp, or ETA", lambda value:
            value["link"] == 1 and value["last_report_ms"] == last_telemetry and
            "预计" not in value["text"].get("layer_lbl", ""))
    s.screenshot("stale-ack-stays-disconnected")
    s.send({"gcode_state": "RUNNING", "mc_percent": 93})
    s.check("fresh telemetry revives fresh remaining estimate", lambda value:
            "预计" in value["text"].get("layer_lbl", ""))
    s.send({"gcode_state": "RUNNING", "mc_percent": 94}, advance=44000)
    s.check("remaining estimate expires at 120 seconds despite fresh telemetry", lambda value:
            value["now_ms"] - value["remaining_updated_ms"] == 120000 and
            "预计" not in value["text"].get("layer_lbl", ""))

    s.send({"gcode_state": "FINISH", "print_error": 0xFFFFFFFF,
            "subtask_name": "current-bracket.3mf", "mc_percent": 100,
            "mc_remaining_time": 7})
    error = s.check("new full32 error is immediate and wins over pending completion", lambda value:
                    value["alert_kind"] == 2 and value["page"] == 3 and
                    value["error_seen"] and value["finish_seen"] and
                    value["print_error"] == 0xFFFFFFFF and
                    value["text"].get("finish_title") == "打印出错" and
                    value["text"].get("finish_sub") == "错误码 0xFFFFFFFF" and
                    value["text"].get("finish_task") == "current-bracket.3mf" and
                    value["remain_min"] == -1 and "预计" not in value["text"].get("layer_lbl", ""))
    generation = error["error_generation"]
    s.screenshot("error-full32-priority")
    s.send({"gcode_state": "FINISH", "print_error": 0xF123ABCD})
    s.check("new error code refreshes displayed generation", lambda value:
            value["alert_kind"] == 2 and value["error_generation"] == generation + 1 and
            value["alert_generation"] == value["error_generation"] and
            value["text"].get("finish_sub") == "错误码 0xF123ABCD")
    s.tap("finish_ok", "finish_ok")
    s.check("acknowledging only current error preserves pending finish", lambda value:
            not value["error_seen"] and value["finish_seen"] and
            value["alert_kind"] == 1 and value["page"] == 3 and
            value["text"].get("finish_title") == "打印完成")
    s.screenshot("pending-finish-after-error-ack")
    s.tap("finish_ok", "finish_ok")
    s.quiet("finish acknowledged separately")
    generations = (s.latest["error_generation"], s.latest["finish_generation"])
    for _ in range(3):
        s.send({"gcode_state": "FINISH", "print_error": 0xF123ABCD})
    s.quiet("repeated telemetry never re-alerts acknowledged error or finish")
    if (s.latest["error_generation"], s.latest["finish_generation"]) != generations:
        raise AssertionError(f"repeated telemetry changed event generations: {s.latest}")
    s.send({"gcode_state": "RUNNING", "print_error": 0,
            "subtask_name": "next-task.3mf", "mc_remaining_time": 12})
    s.send({"gcode_state": "ERROR", "print_error": 0x80000001})
    s.check("later live ERROR transition shows current task and unsigned code", lambda value:
            value["alert_kind"] == 2 and value["text"].get("finish_task") == "next-task.3mf" and
            value["text"].get("finish_sub") == "错误码 0x80000001" and
            value["remain_min"] == -1 and "预计" not in value["text"].get("layer_lbl", ""))
    s.screenshot("error-next-task")
    s.tap("finish_ok", "finish_ok")
    s.quiet("error acknowledged without fabricating finish")
    s.send({"gcode_state": "IDLE", "print_error": 0, "mc_remaining_time": 99})
    s.check("IDLE terminal state clears remaining estimate", lambda value:
            value["print_state"] == 0 and value["remain_min"] == -1 and
            "预计" not in value["text"].get("layer_lbl", ""))


def pending_alert_snapshots(s: Session):
    s.send({"gcode_state": "RUNNING", "subtask_name": "old-job.3mf", "print_error": 0})
    s.send({"gcode_state": "FINISH", "print_error": 134})
    s.check("old task error has priority over its pending completion", lambda value:
            value["alert_kind"] == 2 and value["finish_seen"] and
            value["text"].get("finish_task") == "old-job.3mf" and
            value["text"].get("finish_sub") == "错误码 0x00000086")
    s.send({"gcode_state": "RUNNING", "subtask_name": "new-job.3mf", "print_error": 0})
    s.check("new task does not mutate pending error payload", lambda value:
            value["task"] == "new-job.3mf" and value["print_error"] == 0 and
            value["error_task_snapshot"] == "old-job.3mf" and
            value["alert_error_code"] == 134 and
            value["text"].get("finish_task") == "old-job.3mf" and
            value["text"].get("finish_sub") == "错误码 0x00000086")
    s.tap("finish_ok", "finish_ok")
    s.check("pending old completion uses captured task while new task runs", lambda value:
            value["print_state"] == 3 and value["task"] == "new-job.3mf" and
            value["alert_kind"] == 1 and not value["error_seen"] and
            value["text"].get("finish_title") == "打印完成" and
            value["text"].get("finish_task") == "old-job.3mf")
    s.screenshot("pending-old-finish-after-new-task")
    s.tap("finish_ok", "finish_ok")
    s.quiet("both old alerts dismissed while new task remains running")
    s.advance(1000)
    s.tap("link_rst_btn", "link_rst")
    s.check("restart clears old alert generations and retains monotonic clock", lambda value:
            value["page"] == 0 and value["alert_kind"] == 0 and
            value["alert_generation"] == 0 and value["error_generation"] == 0 and
            value["finish_generation"] == 0 and value["now_ms"] == 1000)
    s.send({"gcode_state": "RUNNING", "subtask_name": "restarted-job.3mf"})
    s.send({"gcode_state": "ERROR", "print_error": 135})
    s.check("new session generation one is displayed after previous generation one", lambda value:
            value["alert_kind"] == 2 and value["alert_generation"] == 1 and
            value["error_generation"] == 1 and
            value["text"].get("finish_task") == "restarted-job.3mf" and
            value["text"].get("finish_sub") == "错误码 0x00000087")
    s.screenshot("new-session-error-generation-one")


def keypad_feedback(s: Session):
    s.tap("set_btn", "set_open")
    s.tap("set_ip_btn", "set_ip")
    s.tap("pad_k_clr", "pad_clr")
    s.tap("pad_k_1", "pad_1")
    s.tap("pad_k_ok", "pad_ok")
    s.check("invalid IPv4 feedback is visible on keypad and preserves input", lambda value:
            value["page"] == 2 and value["pad_len"] == 1 and
            value["text"].get("pad_field") == "1" and
            value["text"].get("pad_error") == "IP 无效")
    s.screenshot("invalid-ip-visible")
    s.tap("pad_k_dot", "pad_dot")
    s.check("editing clears stale keypad validation message", lambda value:
            value["page"] == 2 and value["text"].get("pad_field") == "1." and
            value["text"].get("pad_error") == "")
    s.tap("pad_k_cancel", "pad_cancel")
    s.tap("set_code_btn", "set_code")
    s.tap("pad_k_clr", "pad_clr")
    s.tap("pad_k_ok", "pad_ok")
    s.check("empty access code feedback is visible on keypad", lambda value:
            value["page"] == 2 and value["pad_len"] == 0 and
            value["text"].get("pad_field") == "" and
            value["text"].get("pad_error") == "访问码不能为空")
    s.screenshot("empty-code-visible")
    for key in "ab12":
        s.tap("pad_k_" + key, "pad_" + key)
    s.tap("pad_k_ok", "pad_ok")
    s.check("valid alphabetic access code applies to settings", lambda value:
            value["page"] == 1 and value["text"].get("set_code_lbl") == "访问码  ab12")
    s.tap("set_save_btn", "set_save")
    s.check("save uses restart hook and leaves settings responsive", lambda value:
            value["page"] == 1 and
            value["text"].get("set_status_lbl") == "已保存，正在连接…")
    if not any(item["event"] == "restart" for item in records(s.transport)):
        raise AssertionError("save did not invoke transport restart hook")
    s.screenshot("saved-access-code")
    s.tap("set_back_btn", "set_back")
    s.check("keypad cancellation returns to status", lambda value: value["page"] == 0)


def command_lifecycle(s: Session):
    s.send({"gcode_state": "RUNNING", "subtask_name": "control-job.3mf"}, advance=1000)
    s.tap("qk_pause_btn", "qk_pause")
    first = s.check("pause request is pending with actual sequence and sent bit", lambda value:
                    value["ctrl_busy"] and value["command_parts"] == 1 and
                    value["command_sent_mask"] == 1 and value["command_sequence"][0] > 0)
    first_sequence = first["command_sequence"][0]
    s.advance(8001)
    s.check("shared monotonic command deadline reports timeout", lambda value:
            not value["ctrl_busy"] and
            value["text"].get("prog_hint") == "无响应，请检查打印机")
    before = s.latest["tick"]
    s.check("timed-out host command never receives a delayed synthetic ACK", lambda value:
            not value["ctrl_busy"] and value["print_state"] == 3 and
            value["text"].get("prog_hint") == "无响应，请检查打印机", after=before + 15)
    if any(item["event"] == "ack" for item in records(s.transport)):
        raise AssertionError("expired command produced synthetic ACK")
    s.send({"gcode_state": "RUNNING", "mc_percent": 1})
    s.tap("qk_pause_btn", "qk_pause")
    s.check("new command can begin after timeout", lambda value: value["ctrl_busy"])
    s.tap("link_rst_btn", "link_rst")
    now = s.now
    s.check("reconnect cancels pending request without resetting monotonic clock", lambda value:
            not value["ctrl_busy"] and value["now_ms"] == now and
            value["text"].get("prog_hint") == "已取消")
    before = s.latest["tick"]
    s.check("reconnect cannot replay canceled command", lambda value:
            not value["ctrl_busy"] and value["print_state"] == 3, after=before + 15)
    if any(item["event"] == "ack" for item in records(s.transport)):
        raise AssertionError("canceled command produced synthetic ACK")
    s.send({"gcode_state": "RUNNING", "mc_percent": 2})
    s.tap("qk_pause_btn", "qk_pause")
    pending = s.check("fresh request after reconnect has new sequence", lambda value:
                      value["ctrl_busy"] and value["command_sequence"][0] != first_sequence)
    sequence = pending["command_sequence"][0]
    s.deliver({"system": {"command": "ledctrl", "sequence_id": str(sequence),
                          "result": "SUCCESS"}})
    s.check("same sequence in wrong ACK family cannot confirm pause", lambda value:
            value["ctrl_busy"] and value["command_ack_mask"] == 0)
    s.deliver({"print": {"command": "pause", "sequence_id": str(sequence),
                         "result": "FAIL", "reason": "denied:LAN"}})
    s.check("matching rejection is visible with printer reason", lambda value:
            not value["ctrl_busy"] and
            value["text"].get("prog_hint", "").startswith("被拒绝:") and
            "denied:LAN" in value["text"].get("prog_hint", ""))
    s.screenshot("command-rejected")


def reviewed_regressions(s: Session):
    s.send({"gcode_state": "PREPARE", "subtask_name": "job-A.3mf"}, advance=1000)
    s.check("PREPARE copy uses complete baked vocabulary", lambda value:
            value["text"].get("state_lbl") == "状态  准备中")
    s.screenshot("prepare-baked-glyphs")
    s.send({"gcode_state": "RUNNING", "subtask_name": "job-A.3mf",
            "total_layers": 0, "total_layer_num": 210,
            "mc_percent": -1, "print_percent": 42,
            "mc_remaining_time": 10, "ignored_large_report": "x" * 12000})
    s.check("12KB report reaches shared parser and fallback fields reach UI", lambda value:
            "210" in value["text"].get("layer_lbl", "") and value["print_state"] == 3)
    s.tap("qk_ctrl_btn", "ctrl_open")
    s.tap("ctrl_stop_btn", "ctrl_stop")
    s.check("stop confirmation initially belongs to job A", lambda value: value["stop_armed"])
    s.send({"gcode_state": "RUNNING", "subtask_name": "job-B.3mf"})
    s.check("task change clears old stop confirmation", lambda value:
            not value["stop_armed"] and value["text"].get("ctrl_stop_btn_lbl") == "停止打印")
    s.tap("ctrl_stop_btn", "ctrl_stop")
    s.check("job B requires a fresh confirmation without sending stop", lambda value:
            value["stop_armed"] and not value["ctrl_busy"])
    if any(row["event"] == "command" and row["value"] == 2 for row in records(s.transport)):
        raise AssertionError("task switch reused the old stop confirmation")
    s.tap("ctrl_back_btn", "ctrl_back")
    s.send({"gcode_state": "RUNNING", "ams": {"ams": [
        {"id": 0, "humidity": None, "temp": None,
         "tray": [{"id": 0, "remain": 70, "tray_type": "PLA-AERO"}]},
        {"id": 1, "humidity": "4", "temp": "27.6",
         "tray": [{"id": 0, "remain": 60, "tray_type": "ASA-AERO"}]}]}})
    s.tap("qk_ams_btn", "ams_open")
    s.check("AMS material names and later-unit readings are complete", lambda value:
            value["text"].get("ams_type_lbl_0") == "PLA-AERO" and
            value["text"].get("ams_type_lbl_4") == "ASA-AERO" and
            value["text"].get("ams_humidity_lbl") == "湿度 4" and
            value["text"].get("ams_temp_lbl") == "温度 28°")
    s.screenshot("ams-full-material-names")
    s.tap("ams_back_btn", "ams_back")
    s.send({"gcode_state": "FAILED", "print_error": 0})
    s.check("FAILED without error code raises terminal error dialog", lambda value:
            value["print_state"] == 6 and value["alert_kind"] == 2 and
            value["text"].get("finish_title") == "打印出错" and
            value["text"].get("finish_task") == "job-B.3mf" and value["remain_min"] == -1)
    s.screenshot("failed-state-dialog")
    s.tap("finish_ok", "finish_ok")
    s.quiet("FAILED alert dismissal remains stable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--sim", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", type=Path)
    parser.add_argument("--case", action="append", dest="selected_cases",
                        help="Run only this named scenario; repeat for several scenarios")
    parser.add_argument("--dynamic-font", action="store_true",
                        help="Use a host TTF; default exercises the baked device fonts")
    args = parser.parse_args()
    args.project = args.project.resolve()
    args.sim = args.sim.resolve()
    args.output = args.output.resolve()
    args.backend = (args.backend or args.project / "build-sim/bambu_backend.exe").resolve()
    assets = args.project / "build-sim/bambu_backend_assets"
    args.bundle = assets / "app.gspb"
    args.sidecar = assets / "bundle_gsp.api.json"
    args.font = args.project / "ui/fonts/DengBmb.ttf"
    for path in (args.sim, args.backend, args.bundle, args.sidecar, args.font):
        if not path.is_file():
            parser.error(f"required built artifact missing: {path}")
    simulator_capabilities = json.loads(subprocess.check_output(
        [str(args.sim), "--capabilities"], timeout=15))
    if simulator_capabilities.get("runtime_version") != "1.5.1":
        parser.error(f"expected simulator 1.5.1: {simulator_capabilities}")
    # A new evidence directory prevents reusing stale files from any old run.
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"status": "running", "simulator": str(args.sim),
              "backend": str(args.backend), "bundle": str(args.bundle),
              "bundle_sha256": hashlib.sha256(args.bundle.read_bytes()).hexdigest(),
              "backend_sha256": hashlib.sha256(args.backend.read_bytes()).hexdigest(),
              "tests": [], "printer_transport": "host fixture only",
              "font_mode": "dynamic TTF" if args.dynamic_font else "baked bundle"}
    cases = [("initial-error", {"gcode_state": "ERROR", "print_error": 0xFFFFFFFF,
                                "subtask_name": "historical-error.3mf"},
              lambda s: initial_snapshot(s, "ERROR")),
             ("initial-finish", {"gcode_state": "FINISH", "subtask_name": "historical-finish.3mf"},
              lambda s: initial_snapshot(s, "FINISH")),
             ("live-feature-batch", {"gcode_state": "IDLE", "print_error": 0}, live_feature_batch),
             ("pending-alert-snapshots", {"gcode_state": "IDLE", "print_error": 0}, pending_alert_snapshots),
             ("keypad-feedback", {"gcode_state": "IDLE", "print_error": 0}, keypad_feedback),
             ("command-lifecycle", {"gcode_state": "IDLE", "print_error": 0}, command_lifecycle),
             ("reviewed-regressions", {"gcode_state": "IDLE", "print_error": 0}, reviewed_regressions)]
    if args.selected_cases:
        unknown = set(args.selected_cases) - {name for name, _, _ in cases}
        if unknown:
            parser.error(f"unknown cases: {sorted(unknown)}")
        cases = [case for case in cases if case[0] in args.selected_cases]
    for name, initial, exercise in cases:
        session = Session(args, name, initial)
        error = None
        try:
            session.start()
            exercise(session)
        except Exception:
            error = traceback.format_exc()
            print(f"FAIL {name}: {error}", flush=True)
        finally:
            session.close()
        result["tests"].append(session.result(error))
        print(f"{session.result(error)['status'].upper()} {name}: {len(session.checks)} checks", flush=True)
        atomic_text(args.output / "results.json", json.dumps(result, ensure_ascii=False, indent=2))
    result["status"] = "pass" if all(case["status"] == "pass" for case in result["tests"]) else "fail"
    result["check_count"] = sum(len(case["checks"]) for case in result["tests"])
    atomic_text(args.output / "results.json", json.dumps(result, ensure_ascii=False, indent=2))
    print(f"{result['status'].upper()}: {result['check_count']} checks; {args.output / 'results.json'}")
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
