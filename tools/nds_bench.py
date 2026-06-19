#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BENCH = ROOT / "bench"
NDS = ROOT / "nds"
ROM = NDS / "uzstd_nds_bench.nds"
LOCAL_LIBRETRO_CORES = [
    ROOT / ".bench" / "melonds-ds-dist" / "cores" / "melondsds_libretro.dylib",
    ROOT / ".bench" / "melonds-ds-dist" / "cores" / "melondsds_libretro.so",
]
LOCAL_LIBRETRO_RUNNER = ROOT / ".bench" / "libretro_nds_runner"


def run(cmd, *, cwd=ROOT, env=None, timeout=None, check=True):
    started = time.time()
    proc = subprocess.run(
        [str(c) for c in cmd],
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    elapsed = time.time() - started
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return {
        "cmd": [str(c) for c in cmd],
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "elapsed_seconds": elapsed,
    }


def which(name):
    return shutil.which(name)


def version(cmd):
    try:
        out = run(cmd, timeout=10, check=False)
    except Exception as exc:
        return {"error": str(exc)}
    text = (out["stdout"] or out["stderr"]).strip().splitlines()
    return {
        "cmd": out["cmd"],
        "returncode": out["returncode"],
        "first_line": text[0] if text else "",
    }


def tool_env():
    env = os.environ.copy()
    devkitpro = env.get("DEVKITPRO") or "/opt/devkitpro"
    devkitarm = env.get("DEVKITARM") or str(Path(devkitpro) / "devkitARM")
    env["DEVKITPRO"] = devkitpro
    env["DEVKITARM"] = devkitarm
    env["PATH"] = (
        str(Path(devkitarm) / "bin")
        + os.pathsep
        + str(Path(devkitpro) / "tools" / "bin")
        + os.pathsep
        + env.get("PATH", "")
    )
    return env


def discover_tools(env):
    libretro_core = next((str(p) for p in LOCAL_LIBRETRO_CORES if p.exists()), None)
    paths = {
        "make": which("make"),
        "cc": which("cc"),
        "python3": which("python3"),
        "arm-none-eabi-gcc": shutil.which("arm-none-eabi-gcc", path=env["PATH"]),
        "ndstool": shutil.which("ndstool", path=env["PATH"]),
        "melondsds_libretro": libretro_core,
        "melonDS": which("melonDS") or which("melonds"),
        "DeSmuME": which("desmume-cli") or which("desmume"),
    }
    app_candidates = [
        Path("/Applications/melonDS.app/Contents/MacOS/melonDS"),
        Path.home() / "Applications/melonDS.app/Contents/MacOS/melonDS",
        Path("/Applications/DeSmuME.app/Contents/MacOS/DeSmuME"),
        Path.home() / "Applications/DeSmuME.app/Contents/MacOS/DeSmuME",
    ]
    for candidate in app_candidates:
        if candidate.exists():
            if "melonDS" in candidate.name or "melonDS" in str(candidate):
                paths["melonDS"] = str(candidate)
            else:
                paths["DeSmuME"] = str(candidate)
    return paths


def host_sanity():
    exe = BENCH / "uzstd_host_sanity"
    BENCH.mkdir(exist_ok=True)
    build = run(["cc", "-std=c99", "-O2", "uzstd.c", "-o", exe])
    test = run([exe])
    return {"build": build, "test": test}


def build_rom(env):
    return run(["make", "-C", NDS], env=env, timeout=120)


def build_libretro_runner():
    LOCAL_LIBRETRO_RUNNER.parent.mkdir(exist_ok=True)
    cmd = [
        "cc",
        "-std=c99",
        "-O2",
        "-Wall",
        "-Wextra",
        "-pedantic",
        "tools/libretro_nds_runner.c",
        "-o",
        LOCAL_LIBRETRO_RUNNER,
    ]
    if sys.platform != "darwin":
        cmd.append("-ldl")
    return run(cmd, timeout=60)


def install_homebrew_emulator():
    if not which("brew"):
        return {"status": "skipped", "reason": "brew not found"}
    attempts = []
    for name in ("melonds", "desmume"):
        attempts.append(run(["brew", "install", name], timeout=600, check=False))
        if attempts[-1]["returncode"] == 0:
            return {"status": "installed", "package": name, "attempts": attempts}
    return {"status": "failed", "attempts": attempts}


def launch_emulator(tools, timeout_seconds):
    if tools.get("melondsds_libretro"):
        system_dir = ROOT / ".bench" / "libretro-system"
        save_dir = ROOT / ".bench" / "libretro-save"
        system_dir.mkdir(parents=True, exist_ok=True)
        save_dir.mkdir(parents=True, exist_ok=True)
        build = build_libretro_runner()
        env = os.environ.copy()
        env["LIBRETRO_SYSTEM_DIR"] = str(system_dir)
        env["LIBRETRO_SAVE_DIR"] = str(save_dir)
        out = run(
            [LOCAL_LIBRETRO_RUNNER, tools["melondsds_libretro"], ROM, "1800"],
            env=env,
            timeout=max(timeout_seconds, 30),
            check=False,
        )
        out["status"] = "passed" if out["returncode"] == 0 else "failed"
        out["core"] = tools["melondsds_libretro"]
        out["runner_build"] = build
        try:
            out["parsed_json"] = json.loads(out["stdout"])
        except Exception:
            pass
        return out

    emulator = tools.get("melonDS") or tools.get("DeSmuME")
    if not emulator:
        return {"status": "skipped", "reason": "no melonDS or DeSmuME executable found"}

    cmd = [emulator, str(ROM)]
    try:
        out = run(cmd, timeout=timeout_seconds, check=False)
        out["status"] = "completed"
        return out
    except subprocess.TimeoutExpired as exc:
        return {
            "status": "timeout",
            "cmd": cmd,
            "timeout_seconds": timeout_seconds,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "",
            "note": "GUI emulators often need manual screenshot/transcription of the final DONE screen.",
        }


def write_results(payload):
    BENCH.mkdir(exist_ok=True)
    json_path = BENCH / "nds_results.json"
    md_path = BENCH / "nds_results.md"
    json_path.write_text(json.dumps(payload, indent=2) + "\n")

    tools = payload.get("tools", {})
    versions = payload.get("versions", {})
    lines = [
        "# Nintendo DS uzstd benchmark",
        "",
        "These are emulator-oriented benchmark artifacts. DS hardware timings should be treated as a separate measurement.",
        "",
        "## Status",
        "",
        f"- Host sanity: {payload.get('host_sanity_status', 'not run')}",
        f"- DS build: {payload.get('build_status', 'not run')}",
        f"- Emulator run: {payload.get('emulator_status', 'not run')}",
        f"- ROM: `{ROM}`" if ROM.exists() else f"- ROM: not produced (`{ROM}`)",
        "",
        "## Tools",
        "",
    ]
    for key in sorted(tools):
        lines.append(f"- {key}: `{tools[key]}`" if tools[key] else f"- {key}: not found")
    lines.extend(["", "## Versions", ""])
    for key in sorted(versions):
        first = versions[key].get("first_line", "") if isinstance(versions[key], dict) else ""
        lines.append(f"- {key}: {first or 'unavailable'}")
    lines.extend(["", "## Notes", ""])
    for note in payload.get("notes", []):
        lines.append(f"- {note}")
    if payload.get("emulator", {}).get("stdout"):
        lines.extend(["", "## Emulator stdout", "", "```", payload["emulator"]["stdout"].strip(), "```"])
    if payload.get("emulator", {}).get("stderr"):
        lines.extend(["", "## Emulator stderr", "", "```", payload["emulator"]["stderr"].strip(), "```"])
    md_path.write_text("\n".join(lines).rstrip() + "\n")
    return json_path, md_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-host", action="store_true", help="skip native uzstd sanity test")
    ap.add_argument("--skip-build", action="store_true", help="skip make -C nds")
    ap.add_argument("--skip-emulator", action="store_true", help="do not launch an emulator")
    ap.add_argument("--install-homebrew-emulator", action="store_true", help="try brew install melonds/desmume")
    ap.add_argument("--emulator-timeout", type=int, default=20, help="seconds to wait before stopping a CLI emulator")
    args = ap.parse_args()

    env = tool_env()
    payload = {
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "devkitpro": env["DEVKITPRO"],
        "devkitarm": env["DEVKITARM"],
        "notes": [],
    }

    try:
        if args.install_homebrew_emulator:
            payload["homebrew_emulator_install"] = install_homebrew_emulator()

        tools = discover_tools(env)
        payload["tools"] = tools
        payload["versions"] = {}
        for name in ("arm-none-eabi-gcc", "ndstool", "melondsds_libretro", "melonDS", "DeSmuME"):
            if tools.get(name):
                if name == "melondsds_libretro":
                    payload["versions"][name] = {
                        "first_line": "melonDS DS libretro core dylib",
                        "path": tools[name],
                    }
                else:
                    payload["versions"][name] = version([tools[name], "--version"])

        if not args.skip_host:
            payload["host_sanity"] = host_sanity()
            payload["host_sanity_status"] = "passed"
        else:
            payload["host_sanity_status"] = "skipped"

        if not args.skip_build:
            if not tools.get("arm-none-eabi-gcc") or not tools.get("ndstool"):
                missing = [k for k in ("arm-none-eabi-gcc", "ndstool") if not tools.get(k)]
                payload["build_status"] = "skipped"
                payload["notes"].append("DS build skipped because required devkitPro tools are missing: " + ", ".join(missing))
                payload["notes"].append("Install devkitPro pacman plus the nds-dev package, then rerun tools/nds_bench.py.")
            else:
                payload["build"] = build_rom(env)
                payload["build_status"] = "passed" if ROM.exists() else "failed: ROM not found"
        else:
            payload["build_status"] = "skipped"

        if not args.skip_emulator:
            if ROM.exists():
                payload["emulator"] = launch_emulator(tools, args.emulator_timeout)
                payload["emulator_status"] = payload["emulator"].get("status", "unknown")
                if payload["emulator_status"] in ("timeout", "completed"):
                    payload["notes"].append("The DS console table is visible in the emulator; automated text capture is emulator-dependent.")
            else:
                payload["emulator_status"] = "skipped"
                payload["notes"].append("Emulator run skipped because the ROM was not produced.")
        else:
            payload["emulator_status"] = "skipped"

    except Exception as exc:
        payload["error"] = str(exc)
        payload.setdefault("notes", []).append("Runner stopped after an error; see nds_results.json for captured output.")
        write_results(payload)
        print(exc, file=sys.stderr)
        return 1

    json_path, md_path = write_results(payload)
    print(f"wrote {json_path}")
    print(f"wrote {md_path}")
    if payload.get("error"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
