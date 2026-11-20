#!/usr/bin/env python3
"""Compile and run the portable Bambu regressions with fresh executables."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory")
    args = parser.parse_args()
    project = args.project.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cases = {
        "report": ["bmb_state.c", "bmb_command.c", "bmb_json.c", "bmb_report.c"],
        "command": ["bmb_state.c", "bmb_command.c"],
        "rx": ["bmb_rx.c"],
        "transport_policy": ["bmb_state.c", "bmb_command.c", "bmb_transport_policy.c",
                             "bmb_rx.c", "bmb_report.c", "bmb_json.c"],
    }
    results = {"status": "pass", "project": str(project), "cases": [], "inputs": {}}
    for name, modules in cases.items():
        sources = [project / "main" / module for module in modules]
        sources.append(project / "pc/tests" / f"test_bmb_{name}.c")
        exe = output / (f"test_bmb_{name}.exe" if sys.platform == "win32" else f"test_bmb_{name}")
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-ftrapv",
                   "-I", str(project / "main"), *map(str, sources), "-o", str(exe)]
        for path in [*sources, *(project / "main").glob("bmb_*.h")]:
            results["inputs"][str(path.relative_to(project))] = hashlib.sha256(path.read_bytes()).hexdigest()
        case = {"name": name, "status": "fail", "compile": command}
        try:
            compiled = subprocess.run(command, capture_output=True, timeout=60)
            (output / f"{name}-compile.log").write_bytes(compiled.stdout + compiled.stderr)
            case["compile_exit"] = compiled.returncode
            if compiled.returncode == 0:
                run = subprocess.run([str(exe)], capture_output=True, timeout=15)
                (output / f"{name}.log").write_bytes(run.stdout + run.stderr)
                case["run_exit"] = run.returncode
                case["status"] = "pass" if run.returncode == 0 else "fail"
                print(run.stdout.decode("utf-8", errors="replace"), end="", flush=True)
            else:
                print(compiled.stderr.decode("utf-8", errors="replace"), flush=True)
        except (subprocess.TimeoutExpired, OSError) as error:
            case["error"] = str(error)
            print(f"FAIL {name}: {error}", flush=True)
        results["cases"].append(case)
        if case["status"] != "pass":
            results["status"] = "fail"
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"Native regressions: {results['status']}; evidence={output}", flush=True)
    return 0 if results["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
