"""Watch for Farever.exe and run the full attach pipeline.

Run once and leave it open. Whenever the game starts (new PID), the
script:

  1. Kills any leftover find_me.py from a prior session.
  2. Spawns `find_me.py --loop` so research/live_position.json gets
     pumped fresh.
  3. Waits for live_position.json to receive a write fresh-er than
     when we started (= find_me locked onto the local Hero).
  4. Runs build/injector/RelWithDebInfo/inject.exe against
     build/minimap-dll/RelWithDebInfo/minimap.dll.

When the game closes the watcher tears down find_me. When the game
restarts (different PID) it goes through the whole loop again. Ctrl-C
to quit; the find_me child is killed on exit.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

PROJ      = Path(r"D:\farevermod")
FIND_ME   = PROJ / "tools" / "find_me.py"
INJECT_EX = PROJ / "build" / "injector" / "RelWithDebInfo" / "inject.exe"
DLL_PATH  = PROJ / "build" / "minimap-dll" / "RelWithDebInfo" / "minimap.dll"
LIVE_FILE = PROJ / "research" / "live_position.json"

POLL_SEC      = 2.0
WAIT_LOCK_SEC = 180.0


def farever_pid() -> int | None:
    """Return PID of running Farever.exe, or None. Multiple instances
    aren't supported by the game so we take the first."""
    cmd = ("powershell", "-NoProfile", "-Command",
           "(Get-Process -Name Farever -ErrorAction SilentlyContinue).Id")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           check=False, timeout=5)
    except subprocess.TimeoutExpired:
        return None
    if r.returncode != 0:
        return None
    out = r.stdout.strip()
    if not out:
        return None
    try:
        return int(out.splitlines()[0])
    except ValueError:
        return None


def kill_proc(p: subprocess.Popen | None, label: str) -> None:
    if p is None:
        return
    if p.poll() is not None:
        return
    print(f"auto_attach: terminating {label} (pid {p.pid})")
    try:
        p.terminate()
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    except Exception:
        pass


def wait_for_lockon(start_mtime: float) -> bool:
    """Block until live_position.json is written fresher than
    `start_mtime` AND contains an x coordinate, or until timeout."""
    deadline = time.time() + WAIT_LOCK_SEC
    while time.time() < deadline:
        try:
            st = LIVE_FILE.stat()
            if st.st_mtime > start_mtime:
                data = json.loads(LIVE_FILE.read_text())
                if "x" in data:
                    return True
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(POLL_SEC)
    return False


def inject() -> None:
    print(f"auto_attach: injecting {DLL_PATH.name}")
    r = subprocess.run([str(INJECT_EX), str(DLL_PATH)],
                       capture_output=True, text=True, check=False)
    msg = (r.stdout or "").strip() or (r.stderr or "").strip()
    if msg:
        print(f"  {msg}")
    if r.returncode != 0:
        print(f"  inject failed: exit {r.returncode}")


def attach_cycle() -> int:
    """One run: wait for Farever, do find_me + inject, then wait for
    Farever to exit. Returns 0 on clean shutdown."""
    finder: subprocess.Popen | None = None
    last_pid: int | None = None

    try:
        print("auto_attach: watching Farever.exe ...")
        while True:
            pid = farever_pid()

            if pid is None:
                if last_pid is not None:
                    print(f"auto_attach: Farever (pid {last_pid}) gone")
                    kill_proc(finder, "find_me")
                    finder = None
                    last_pid = None
                time.sleep(POLL_SEC)
                continue

            if pid == last_pid:
                # Check the finder is still alive; if it crashed,
                # restart it without re-injecting.
                if finder and finder.poll() is not None:
                    print("auto_attach: find_me died — restarting")
                    start_mtime = (LIVE_FILE.stat().st_mtime
                                   if LIVE_FILE.exists() else 0.0)
                    finder = subprocess.Popen(
                        [sys.executable, str(FIND_ME), "--loop"],
                        cwd=str(PROJ / "tools"))
                    if not wait_for_lockon(start_mtime):
                        print("auto_attach: lockon timeout (restart)")
                time.sleep(POLL_SEC)
                continue

            # New PID detected.
            print(f"auto_attach: Farever pid={pid} detected")
            last_pid = pid
            kill_proc(finder, "find_me")
            finder = None

            start_mtime = (LIVE_FILE.stat().st_mtime
                           if LIVE_FILE.exists() else 0.0)

            print("auto_attach: starting find_me.py --loop")
            finder = subprocess.Popen(
                [sys.executable, str(FIND_ME), "--loop"],
                cwd=str(PROJ / "tools"))

            print("auto_attach: waiting for fresh live_position.json ...")
            if not wait_for_lockon(start_mtime):
                print("auto_attach: lockon timeout — skipping inject")
                continue

            inject()
    except KeyboardInterrupt:
        print("\nauto_attach: stopping")
    finally:
        kill_proc(finder, "find_me")
    return 0


if __name__ == "__main__":
    raise SystemExit(attach_cycle())
