#!/usr/bin/env python3
"""Proves the display comes back to GDI after a fullscreen D3D app is gone.

The driver publishes frames to the host; the host shows whichever span was
published last.  Only DirectDraw publishes, so when the final exclusive owner
disappears without the driver closing its D3D transport session, the host keeps
scanning out a dead flip-chain member forever: Windows keeps painting the
desktop into video memory and none of it reaches the screen.  The guest stays
fully alive, which makes the failure look like a hung game rather than a driver
defect.

The cycle boots to the desktop, starts the fullscreen game, waits for it to own
the screen, kills it by logging off, and requires the desktop to be visible
again.  A regression here freezes the screen on the game's last frame.

Usage: python display-return-cycle.py --cmdline <name> --frontend retro-host
                                      --retro-host <exe> [--qmp-port N]
"""
import argparse
import glob
import importlib.util
import json
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# Two frames this far apart cannot be the same still image.
DISTINCT_MEAN = 5.0
# The game holds a black screen while it loads; its title screen sits near 14.
GAME_RENDER_MEAN = 5.0
# A live shell frame is never pure black; the captured display is.
POST_LIVE_MEAN = 5.0


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


m = load(os.path.join(HERE, "pop-menu-cycle.py"), "popcycle")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmdline", required=True)
    ap.add_argument("--frontend", default="retro-host",
                    choices=("retro-host", "retroarch"))
    ap.add_argument("--retro-host", default=m.RETRO_HOST,
                    help="retro_host.exe path (default: XPD_RETRO_HOST)")
    ap.add_argument("--retroarch", default=m.RETROARCH,
                    help="RetroArch path (default: XPD_RETROARCH)")
    ap.add_argument("--core", default=m.CORE,
                    help="core DLL (default: XPD_CORE, then paired core build-win64)")
    ap.add_argument("--qmp-port", type=int, default=49313)
    ap.add_argument("--wait-ms", type=int, default=900000,
                    help="frontend run budget; the cycle needs the whole boot,"
                         " game, and logoff sequence")
    ap.add_argument("--boot-wait", type=int, default=60)
    ap.add_argument("--own-wait", type=int, default=180,
                    help="seconds allowed for the game to own the screen")
    ap.add_argument("--logoff-wait", type=int, default=60)
    ap.add_argument("--label", default="display-return")
    args = ap.parse_args()
    if args.frontend == "retro-host" and not args.retro_host:
        print("--frontend retro-host requires --retro-host or XPD_RETRO_HOST")
        return 4
    if args.frontend == "retroarch" and not args.retroarch:
        print("--frontend retroarch requires --retroarch or XPD_RETROARCH")
        return 4

    m.QMP_PORT = args.qmp_port
    m.FRONTEND_IMAGE = ("retro_host.exe" if args.frontend == "retro-host"
                        else "retroarch.exe")
    cmdline = os.path.join(m.WINXP, args.cmdline)
    run_dir = os.path.join(REPO, "artifacts", "runs",
                           time.strftime("%Y%m%d-%H%M%S") + "-" + args.label)
    os.makedirs(run_dir, exist_ok=True)
    print("run dir: %s" % run_dir, flush=True)

    def shot(label):
        path = os.path.join(run_dir, "%s.bmp" % label)
        if not m.screendump(path):
            return None
        stats = m.frame_stats(path)
        mean = None if stats is None else stats["mean"]
        print("SHOT %s mean=%s" % (label, mean), flush=True)
        return mean

    if m.retroarch_running():
        print("frontend already running; refusing (disk safety)")
        return 4

    # A stale ring frame from an earlier cycle would be captured as this run's
    # first screenshot.
    for stale in glob.glob(os.path.join(m.WINXP, "frame-ring-*.bmp")):
        try:
            os.remove(stale)
        except OSError:
            pass
    env = dict(os.environ, XPD_FRAME_RING="1")
    if args.frontend == "retro-host":
        popen_args = [args.retro_host, args.core, cmdline,
                      str(args.qmp_port), "--wait", str(args.wait_ms),
                      "--no-quit"]
        popen_kwargs = {"creationflags":
                        getattr(subprocess, "CREATE_NO_WINDOW", 0)}
    else:
        popen_args = [args.retroarch, "-L", args.core, cmdline]
        popen_kwargs = {}
    frontend_log = open(os.path.join(run_dir, "frontend.log"), "w")
    proc = subprocess.Popen(popen_args, cwd=m.WINXP, env=env,
                            stdout=frontend_log, stderr=subprocess.STDOUT,
                            **popen_kwargs)
    verdict = {"verdict": "ERROR", "detail": "", "means": {}}
    try:
        deadline = time.time() + 240
        while time.time() < deadline and not m.qmp_alive():
            if proc.poll() is not None:
                verdict["detail"] = "frontend exited during boot"
                return 4
            time.sleep(3)
        if not m.qmp_alive():
            verdict["detail"] = "QMP never came up"
            return 4
        time.sleep(args.boot_wait)

        boot = os.path.join(run_dir, "boot.bmp")
        ready = False
        for _ in range(160):
            if m.screendump(boot) and m.looks_desktop_ready(boot):
                ready = True
                break
            time.sleep(2)
        if not ready:
            shot("boot-state")
            verdict["detail"] = "desktop never became ready"
            return 4
        desktop = shot("desktop")
        verdict["means"]["desktop"] = desktop

        # Launch through the unique "Play ..." desktop shortcut, then clear the
        # game's own launcher dialog.
        m.sendkeys("meta_l", "d")
        time.sleep(1)
        for key in ("p", "l", "a", "y"):
            m.sendkey(key)
            time.sleep(0.15)
        time.sleep(1)
        m.sendkey("ret")
        time.sleep(8)
        m.sendkeys("shift", "tab")
        time.sleep(1)
        m.sendkey("ret")
        time.sleep(5)
        for _ in range(2):
            m.sendkey("ret")
            time.sleep(5)

        # The game blanks the screen long before it renders, so "not the
        # desktop" is not ownership.  Wait for two consecutive frames that
        # carry real game output.
        owned = None
        owned_frame = None
        rendering = 0
        for index in range(args.own_wait // 10):
            time.sleep(10)
            mean = shot("owned-%02d" % index)
            if (mean is not None and mean >= GAME_RENDER_MEAN and
                    abs(mean - desktop) >= DISTINCT_MEAN):
                owned = mean
                owned_frame = "owned-%02d.bmp" % index
                rendering += 1
                if rendering >= 2:
                    break
            else:
                rendering = 0
        verdict["means"]["owned"] = owned
        if owned is None or rendering < 2:
            verdict["verdict"] = "NO-OWNER"
            verdict["detail"] = "the game never took the screen"
            return 3

        # Logging off destroys the game's process and therefore its DirectDraw
        # objects.  Nothing else can hand the display back.
        m.sendkeys("meta_l", "r")
        time.sleep(1.5)
        for char in "logoff":
            m.sendkey(char)
            time.sleep(0.08)
        m.sendkey("ret")
        time.sleep(args.logoff_wait)
        after = shot("post-logoff")
        verdict["means"]["post_logoff"] = after
        if after is None:
            verdict["verdict"] = "ERROR"
            verdict["detail"] = "no frame after logoff"
            return 4
        # Two ways the display can stay captured, and both have been observed:
        # it never leaves the owner's last frame, or the owner's final black
        # frame is the last thing ever published and the screen stays dark
        # while Windows paints a desktop nobody can see.  A live shell here is
        # the logon screen or the desktop, never pure black.
        delta = m.frame_delta(os.path.join(run_dir, owned_frame),
                              os.path.join(run_dir, "post-logoff.bmp"))
        verdict["owner_frame_delta"] = delta
        if delta < DISTINCT_MEAN or after < POST_LIVE_MEAN:
            verdict["verdict"] = "FROZEN"
            verdict["detail"] = (
                "display never returned to GDI (post mean %.4f, delta %.4f "
                "from %s)" % (after, delta, owned_frame))
            return 2
        verdict["verdict"] = "RETURNED"
        verdict["detail"] = (
            "desktop mean %.4f, game mean %.4f, after logoff %.4f, "
            "delta from owner frame %.4f" % (desktop, owned, after, delta))
        return 0
    finally:
        print("VERDICT %s: %s" % (verdict["verdict"], verdict["detail"]),
              flush=True)
        with open(os.path.join(run_dir, "verdict.json"), "w") as fh:
            json.dump(verdict, fh, indent=2)
        m.shutdown(proc)
        frontend_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
