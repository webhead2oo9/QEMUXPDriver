#!/usr/bin/env python3
"""Install the newest XPDM ISO package into the persistent games VM.

Launches the install cmdline, waits for Explorer, runs
D:\\XPDM\\qxpsetup.exe via Win+R, verifies its serial identity/status,
waits for its automatic reboot and the post-reboot desktop, then powers XP
down cleanly. Artifacts land in artifacts/runs/<stamp>-xpdm-install-<ver>/.
"""
import argparse
import glob
import importlib.util
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "pop_cycle", os.path.join(HERE, "pop-menu-cycle.py"))
pc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pc)


def send_text(text):
    for ch in text.lower():
        if "a" <= ch <= "z" or "0" <= ch <= "9":
            pc.sendkey(ch)
        elif ch == ":":
            pc.sendkeys("shift", "semicolon")
        elif ch == "\\":
            pc.sendkey("backslash")
        elif ch == ".":
            pc.sendkey("dot")
        elif ch == "-":
            pc.sendkey("minus")
        else:
            raise ValueError("unsupported input character: %r" % ch)
        time.sleep(0.08)


def wait_desktop(frame, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pc.screendump(frame) and pc.looks_desktop_ready(frame):
            return True
        time.sleep(2)
    return False


def read_text(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmdline")
    ap.add_argument("--version")
    ap.add_argument("--frontend", choices=["retroarch", "retro-host"],
                    default="retroarch",
                    help="retro-host runs the headless frozen frontend")
    ap.add_argument("--retro-host", default=pc.RETRO_HOST,
                    help="retro_host.exe path (default: XPD_RETRO_HOST)")
    ap.add_argument("--retroarch", default=pc.RETROARCH,
                    help="RetroArch path (default: XPD_RETROARCH)")
    ap.add_argument("--core", default=pc.CORE,
                    help="core DLL (default: XPD_CORE, then paired core build-win64)")
    ap.add_argument("--wait-ms", type=int, default=600000,
                    help="retro_host --wait budget for the whole session")
    ap.add_argument("--qmp-port", type=int, default=pc.QMP_PORT)
    args = ap.parse_args()
    if args.frontend == "retro-host" and not args.retro_host:
        print("--frontend retro-host requires --retro-host or XPD_RETRO_HOST")
        return 4
    if args.frontend == "retroarch" and not args.retroarch:
        print("--frontend retroarch requires --retroarch or XPD_RETROARCH")
        return 4
    pc.QMP_PORT = args.qmp_port
    # The shared helpers key both the "another cycle owns this disk" guard and
    # the shutdown taskkill fallback off this image name.  Leaving it at the
    # RetroArch default while running retro-host lets a second cycle start on
    # a disk that is already mounted, which corrupts the overlay.
    pc.FRONTEND_IMAGE = ("retro_host.exe" if args.frontend == "retro-host"
                         else "retroarch.exe")

    cmdline = os.path.abspath(args.cmdline) if args.cmdline else None
    if not cmdline:
        candidates = sorted(glob.glob(os.path.join(
            pc.WINXP, "xpdriver-xpdm-games-pop-*-install.qemu_cmd_line")))
        if not candidates:
            print("no POP install cmdline found")
            return 4
        cmdline = candidates[-1]
    match = re.search(r"pop-(\d+)-install", os.path.basename(cmdline))
    version = args.version or (match.group(1) if match else "unknown")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(pc.REPO, "artifacts", "runs",
                           "%s-xpdm-install-%s" % (stamp, version))
    os.makedirs(run_dir, exist_ok=True)
    result_log = None
    debug_log = None
    try:
        with open(cmdline, errors="replace") as fh:
            cmdline_text = fh.read()
        serial_match = re.search(r"-serial\s+file:([^\s]+)", cmdline_text)
        debug_match = re.search(r"-debugcon\s+file:([^\s]+)", cmdline_text)
        if serial_match:
            result_log = os.path.join(pc.WINXP, serial_match.group(1))
        if debug_match:
            debug_log = os.path.join(pc.WINXP, debug_match.group(1))
    except OSError:
        pass
    if not result_log:
        result_log = os.path.join(pc.WINXP,
                                  "xpdm-games-pop-%s-result.log" % version)
    if not debug_log:
        debug_log = os.path.join(pc.WINXP,
                                 "xpdm-games-pop-%s-debug.log" % version)

    print("cmdline: %s" % os.path.basename(cmdline))
    print("run dir: %s" % run_dir)
    if pc.retroarch_running():
        print("%s already running; refusing (disk safety)"
              % pc.FRONTEND_IMAGE)
        return 4
    for stale in glob.glob(os.path.join(pc.WINXP, "frame-ring-*.bmp")):
        try:
            os.remove(stale)
        except OSError:
            pass
    for stale in (result_log, debug_log):
        try:
            os.remove(stale)
        except OSError:
            pass

    env = dict(os.environ, XPD_FRAME_RING="1")

    def launch_frontend(log_handle):
        if args.frontend == "retro-host":
            popen_args = [args.retro_host, args.core, cmdline,
                          str(args.qmp_port), "--wait", str(args.wait_ms),
                          "--no-quit"]
            popen_kwargs = {"creationflags":
                            getattr(subprocess, "CREATE_NO_WINDOW", 0)}
        else:
            popen_args = [args.retroarch, "-L", args.core, cmdline]
            popen_kwargs = {}
        return subprocess.Popen(popen_args,
                                cwd=pc.WINXP, env=env, stdout=log_handle,
                                stderr=subprocess.STDOUT, **popen_kwargs)

    frontend_path = os.path.join(run_dir, "frontend.log")
    frontend = open(frontend_path, "w")
    proc = launch_frontend(frontend)
    ok = False
    try:
        deadline = time.time() + 180
        while time.time() < deadline and not pc.qmp_alive():
            if proc.poll() is not None:
                print("frontend exited before QMP")
                return 4
            time.sleep(3)
        if not pc.qmp_alive():
            print("QMP did not become ready")
            return 4

        pre = os.path.join(run_dir, "pre-install-desktop.bmp")
        print("waiting for pre-install desktop", flush=True)
        time.sleep(30)
        if not wait_desktop(pre, 120):
            print("pre-install desktop timeout")
            return 4

        print("launching D:\\XPDM\\qxpsetup.exe", flush=True)
        pc.sendkeys("meta_l", "r")
        time.sleep(1)
        send_text(r"d:\xpdm\qxpsetup.exe")
        pc.sendkey("ret")

        deadline = time.time() + 240
        serial = ""
        while time.time() < deadline:
            serial = read_text(result_log)
            result_codes = re.findall(r"QXPICD_RESULT rc=([0-9]+)",
                                      serial)
            if any(int(code) != 0 for code in result_codes):
                print("qxpsetup reported failure")
                print(serial[-1000:])
                return 4
            if "QXPSETUP stage=rebooting" in serial:
                break
            time.sleep(2)
        if "QXPSETUP stage=rebooting" not in serial:
            pc.screendump(os.path.join(run_dir, "install-timeout.bmp"))
            print("qxpsetup reboot marker timeout")
            print(serial[-1000:])
            return 4
        identity_pattern = (r"QXPSETUP identity driverVer=[^\r\n]*,"
                            r"1\.0\.0\.%s(?: |$)" % re.escape(version))
        if not re.search(identity_pattern, serial):
            print("installed identity mismatch; expected 1.0.0.%s" %
                  version)
            print(serial[-1000:])
            return 4
        print("installer identity verified; waiting for reboot", flush=True)
        with open(os.path.join(run_dir, "installer-result.log"), "w") as fh:
            fh.write(serial)
        if os.path.exists(debug_log):
            with open(debug_log, "rb") as a, open(
                    os.path.join(run_dir, "installer-debug.log"), "wb") as b:
                b.write(a.read())

        # This libretro frontend unloads the QEMU core when XP requests a
        # reboot, then leaves RetroArch's dummy menu running. Wait for that
        # safe core unload, terminate only the dummy frontend, and relaunch
        # the INSTALL cmdline so the RunOnce native test still has D: mounted.
        core_unloaded = False
        deadline = time.time() + 20
        while time.time() < deadline:
            if "Environ SHUTDOWN." in read_text(frontend_path):
                core_unloaded = True
                break
            try:
                status_reply = pc.qmp({"execute": "query-status"},
                                      timeout=5)
                status = status_reply.get("return", {}).get("status", "")
                if status in ("shutdown", "prelaunch"):
                    core_unloaded = True
                    break
            except OSError:
                core_unloaded = True
                break
            time.sleep(2)
        if not core_unloaded:
            # ExitWindowsEx and ACPI powerdown both stall on this frontend.
            # Package files are closed and HKLM was flushed before the serial
            # marker. QMP quit performs the QEMU block-backend close/flush;
            # relaunching the same ISO-mounted cmdline then provides the
            # required paired miniport/display-driver reboot.
            print("guest reboot stalled; requesting QMP quit/relaunch",
                  flush=True)
            try:
                reply = pc.qmp({"execute": "quit"}, timeout=8)
                if "error" in reply:
                    print("QMP quit failed: %s" % reply)
                    return 4
            except OSError:
                pass
            deadline = time.time() + 30
            while time.time() < deadline:
                if ("Environ SHUTDOWN." in read_text(frontend_path) or
                        proc.poll() is not None or not pc.qmp_alive()):
                    core_unloaded = True
                    break
                time.sleep(1)
            if not core_unloaded:
                print("QEMU core did not unload after QMP quit")
                return 4
        if core_unloaded:
            print("QEMU core unloaded; completing reboot in a new frontend",
                  flush=True)
            try:
                proc.terminate()
                proc.wait(timeout=8)
            except (OSError, subprocess.TimeoutExpired):
                subprocess.run(["taskkill", "/IM", "retroarch.exe", "/F"],
                               capture_output=True)
                time.sleep(2)
            frontend.close()
            frontend_path = os.path.join(run_dir,
                                         "post-reboot-frontend.log")
            frontend = open(frontend_path, "w")
            proc = launch_frontend(frontend)
            deadline = time.time() + 180
            while time.time() < deadline and not pc.qmp_alive():
                if proc.poll() is not None:
                    print("post-reboot frontend exited before QMP")
                    return 4
                time.sleep(3)
            if not pc.qmp_alive():
                print("post-reboot QMP timeout")
                return 4

        # Do not accept the stale pre-reboot desktop from the rolling ring.
        time.sleep(30 if core_unloaded else 20)
        for stale in glob.glob(os.path.join(pc.WINXP,
                                            "frame-ring-*.bmp")):
            try:
                os.remove(stale)
            except OSError:
                pass
        post = os.path.join(run_dir, "post-reboot-desktop.bmp")
        if not wait_desktop(post, 180):
            print("post-reboot desktop timeout")
            return 4
        deadline = time.time() + 90
        while time.time() < deadline:
            native_result = read_text(result_log)
            codes = re.findall(r"QXPICD_RESULT rc=([0-9]+)",
                               native_result)
            if codes:
                if codes[-1] != "0":
                    print("post-reboot native test failed: rc=%s" %
                          codes[-1])
                    return 4
                break
            time.sleep(2)
        if not codes:
            print("post-reboot native test result timeout")
            return 4
        print("INSTALL VERIFIED: 1.0.0.%s" % version)
        ok = True
    finally:
        pc.shutdown(proc)
        frontend.close()
        for src in (result_log, debug_log):
            if os.path.exists(src):
                dst = os.path.join(run_dir, os.path.basename(src))
                try:
                    with open(src, "rb") as a, open(dst, "wb") as b:
                        b.write(a.read())
                except OSError:
                    pass
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(main())
