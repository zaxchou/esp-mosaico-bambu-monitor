#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fresh host build of real bmb_ui.c with GSP boundary fault injection; no IDF.

All artifacts (including production host traces) stay in a unique output directory.
Use --assets to select the directory containing the generated bundle_gsp.h and
--gsp-component for the pinned managed_components/espressif__esp-gsp checkout.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(paths: list[Path]) -> dict[str, str]:
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(set(paths))}


def run_logged(command: list[str], output: Path, name: str,
               timeout: int) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(command, cwd=output, capture_output=True, timeout=timeout)
    header = (json.dumps(command, ensure_ascii=False) + "\n\n").encode("utf-8")
    trailer = f"\nexit_code={result.returncode}\n".encode("ascii")
    (output / name).write_bytes(header + result.stdout + result.stderr + trailer)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--assets", type=Path, help="Generated bundle_gsp.h directory")
    parser.add_argument("--gsp-component", type=Path, help="Pinned ESP-GSP 1.5.1 component")
    parser.add_argument("--cc", help="Host GCC executable")
    parser.add_argument("--output", type=Path, help="New, non-existing evidence directory")
    args = parser.parse_args()
    project = args.project.resolve()
    mirror = Path.home() / "esp/work/esp-mosaico-zax/projects/bambu_monitor"
    roots = [project, mirror]
    assets = args.assets or next((root / "build-sim/bambu_backend_assets" for root in roots
                                  if (root / "build-sim/bambu_backend_assets/bundle_gsp.h").is_file()), None)
    gsp = args.gsp_component or next((root / "managed_components/espressif__esp-gsp"
                                      for root in roots
                                      if (root / "managed_components/espressif__esp-gsp/include/esp_gsp.h").is_file()), None)
    if assets is None or gsp is None:
        parser.error("Pass --assets and --gsp-component for existing generated/pinned headers")
    assets, gsp = assets.resolve(), gsp.resolve()
    for path in [assets / "bundle_gsp.h", gsp / "include/esp_gsp.h"]:
        if not path.is_file():
            parser.error(f"Missing required header: {path}")
    cc = args.cc or shutil.which("gcc")
    if cc is None:
        fallback = Path.home() / "esp/tools/w64devkit/bin/gcc.exe"
        if fallback.is_file():
            cc = str(fallback)
    if cc is None:
        parser.error("Host GCC not found; pass --cc")
    if args.output:
        output = args.output.resolve()
        if sys.platform == "win32" and output.drive.upper() != "C:":
            parser.error("Use a fresh C: output directory for host executables and logs")
        output.mkdir(parents=True, exist_ok=False)
    else:
        temp_root = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp"
        temp_root.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32" and temp_root.drive.upper() != "C:":
            parser.error("Pass --output with a fresh directory on C:")
        output = Path(tempfile.mkdtemp(prefix="bmb-ui-startup-", dir=temp_root)).resolve()
    print(f"Evidence: {output}", flush=True)

    sources = [project / "main" / name for name in
               ["bmb_ui.c", "bmb_state.c", "bmb_command.c", "bmb_json.c", "bmb_report.c"]]
    sources.append(project / "pc/tests/test_bmb_ui_startup.c")
    inputs = [*sources, Path(__file__).resolve(),
              *(project / "main").glob("bmb_*.h"), *(project / "pc").glob("*.h"),
              *assets.rglob("*.h"), *(gsp / "include").rglob("*.h")]
    result = {"status": "fail", "project": str(project), "assets": str(assets),
              "gsp_component": str(gsp), "output": str(output), "compiles": []}
    try:
        version_header = (gsp / "include/gsp/gsp_version.h").read_text(encoding="utf-8")
        if '#define GSP_VERSION_STRING "1.5.1"' not in version_header:
            raise ValueError("Expected the pinned ESP-GSP 1.5.1 headers")
        result["inputs_before"] = digest(inputs)
        version = run_logged([cc, "--version"], output, "compiler.log", 15)
        if version.returncode:
            raise RuntimeError("Compiler --version failed; see compiler.log")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-ftrapv",
                 "-finput-charset=UTF-8", "-fexec-charset=UTF-8"]
        include = ["-I", str(project / "main"), "-I", str(project / "pc"),
                   "-I", str(assets), "-I", str(gsp / "include")]
        objects = []
        for source in sources:
            obj = output / (source.stem + ".o")
            command = [cc, *flags, *include, "-MMD", "-MF", str(obj.with_suffix(".d")),
                       "-c", str(source), "-o", str(obj)]
            compiled = run_logged(command, output, source.stem + "-compile.log", 60)
            result["compiles"].append({"source": str(source), "command": command,
                                       "exit_code": compiled.returncode})
            if compiled.returncode:
                print(compiled.stderr.decode("utf-8", errors="replace"), flush=True)
                raise RuntimeError(f"Compile failed: {source.name}")
            objects.append(obj)
        exe = output / ("test_bmb_ui_startup.exe" if sys.platform == "win32" else "test_bmb_ui_startup")
        command = [cc, *map(str, objects), "-o", str(exe)]
        linked = run_logged(command, output, "link.log", 60)
        result["link"] = {"command": command, "exit_code": linked.returncode}
        if linked.returncode:
            print(linked.stderr.decode("utf-8", errors="replace"), flush=True)
            raise RuntimeError("Link failed; see link.log")
        result["executable_sha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
        run = run_logged([str(exe)], output, "test_bmb_ui_startup.log", 30)
        result["run_exit_code"] = run.returncode
        print(run.stdout.decode("utf-8", errors="replace"), end="", flush=True)
        print(run.stderr.decode("utf-8", errors="replace"), end="", flush=True)
        result["inputs_after"] = digest(inputs)
        result["changed_inputs"] = [path for path in result["inputs_before"]
                                    if result["inputs_before"][path] != result["inputs_after"][path]]
        if result["changed_inputs"]:
            raise RuntimeError("Inputs changed during the run; rerun against the latest files")
        result["status"] = "pass" if run.returncode == 0 else "fail"
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
        print(f"FAIL: {error}", flush=True)
    (output / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"UI startup regressions: {result['status']}; evidence={output}", flush=True)
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
