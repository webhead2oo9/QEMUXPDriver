#!/usr/bin/env python3
"""Automated POP:SoT menu test cycle.

Launches the games VM (RetroArch + qemu-libretro core), waits for boot,
launches POP from the desktop (type 'p' to select the icon, Enter to run,
then answer the safe-mode prompt), spams spacebar to skip the intro
cutscenes (the user's manual recipe), then captures a burst of
screendumps at the title screen and classifies:

  ANIMATING  - menu frames differ (presentation pipeline alive)  -> PASS
  STATIC     - non-black frame frozen (presents dropped/stale)   -> FAIL
  BLACK      - all frames black                                  -> FAIL
  DESKTOP    - XP desktop visible (POP never started)            -> SETUP

Artifacts (frames, verdict.json, guest debug log, host traces) land in
artifacts/runs/<stamp>-pop-cycle/.  Exit code: 0 PASS, 2 FAIL, 3 SETUP,
4 infrastructure error.

Usage: python pop-menu-cycle.py [--cmdline <path>] [--boot-wait 100]
                                [--skip-secs 170] [--label text]
No guest changes; QMP only (screendump + sendkey), same instance guard as
launch-games-vm.bat.
"""
import argparse, glob, json, math, os, re, socket, struct, subprocess, sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WINXP = os.path.join(REPO, "qemu", "winxp")
RETROARCH = os.environ.get("XPD_RETROARCH")
RETRO_HOST = os.environ.get("XPD_RETRO_HOST")
# Match guest/paired-core.mk: the in-tree core, else its sibling checkout.
CORE_ROOT = os.path.join(REPO, "core")
if not os.path.isfile(os.path.join(CORE_ROOT, "hw", "mesa", "mglfunci.h")):
    CORE_ROOT = os.path.abspath(os.path.join(REPO, "..", "qemu-libretro-d3d9"))
CORE = os.environ.get("XPD_CORE") or os.path.join(
    CORE_ROOT, "build-win64", "libqemu_libretro.dll")
QMP_PORT = 49055
FRONTEND_IMAGE = "retroarch.exe"


def qmp(cmd, timeout=20):
    s = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=timeout)
    f = s.makefile("rw", encoding="utf-8", newline="\n")
    json.loads(f.readline())
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    json.loads(f.readline())
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = f.readline()
        if not line:
            break
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            s.close()
            return msg
    s.close()
    return {"error": "timeout"}


def qmp_alive():
    try:
        return "return" in qmp({"execute": "query-status"}, timeout=5)
    except OSError:
        return False


_dump_error_logged = False
_key_error_logged = False


def screendump(path):
    # This core build has no QMP screendump; the instrumented core dumps a
    # rolling BMP ring of the frames it hands the frontend
    # (XPD_FRAME_RING=1).  Capture = copy the newest ring slot.
    global _dump_error_logged
    for _ in range(3):
        ring = [path for path in glob.glob(
            os.path.join(WINXP, "frame-ring-*.bmp"))
            if re.fullmatch(r"frame-ring-[0-9]+\.bmp",
                            os.path.basename(path))]
        candidates = []
        for source in ring:
            try:
                candidates.append((os.path.getmtime(source), source))
            except OSError:
                pass
        if candidates:
            newest = max(candidates)[1]
            try:
                with open(newest, "rb") as a:
                    data = a.read()
                if data[:2] == b"BM" and len(data) > 54:
                    with open(path, "wb") as b:
                        b.write(data)
                    return True
            except OSError:
                pass
        time.sleep(0.3)
    if not _dump_error_logged:
        _dump_error_logged = True
        print("no frame-ring files yet")
    return False


def sendkeys(*keys):
    global _key_error_logged
    r = qmp({"execute": "send-key",
             "arguments": {"keys": [
                 {"type": "qcode", "data": key} for key in keys
             ]}})
    if "return" not in r and not _key_error_logged:
        _key_error_logged = True
        print(f"send-key failed: {r}")


def sendkey(key):
    sendkeys(key)


# US layout. Unshifted keys first, then the shifted pairs; anything outside
# these tables raises rather than silently typing the wrong command into a
# live guest.
_PLAIN_KEYS = {
    ":": ("shift", "semicolon"),
    "\\": ("backslash",),
    ".": ("dot",),
    " ": ("spc",),
    '"': ("shift", "apostrophe"),
    "-": ("minus",),
    ",": ("comma",),
    "/": ("slash",),
    ";": ("semicolon",),
    "'": ("apostrophe",),
    "=": ("equal",),
    "[": ("bracket_left",),
    "]": ("bracket_right",),
    "`": ("grave_accent",),
    ">": ("shift", "dot"),
    "<": ("shift", "comma"),
    "?": ("shift", "slash"),
    "_": ("shift", "minus"),
    "+": ("shift", "equal"),
    "|": ("shift", "backslash"),
    "{": ("shift", "bracket_left"),
    "}": ("shift", "bracket_right"),
    "~": ("shift", "grave_accent"),
    "!": ("shift", "1"),
    "@": ("shift", "2"),
    "#": ("shift", "3"),
    "$": ("shift", "4"),
    "%": ("shift", "5"),
    "^": ("shift", "6"),
    "&": ("shift", "7"),
    "*": ("shift", "8"),
    "(": ("shift", "9"),
    ")": ("shift", "0"),
}


def send_text(text):
    """Types text literally, case included.

    Case used to be silently folded to lower, which is harmless for paths but
    quietly corrupts case-sensitive command switches: `ntsd -G` arrived as
    `-g`.
    """
    for char in text:
        lower = char.lower()
        if "a" <= lower <= "z":
            if char == lower:
                sendkey(lower)
            else:
                sendkeys("shift", lower)
        elif "0" <= char <= "9":
            sendkey(char)
        elif char in _PLAIN_KEYS:
            sendkeys(*_PLAIN_KEYS[char])
        else:
            raise ValueError(f"unsupported input character: {char!r}")
        time.sleep(0.08)



def send_mouse_move_click():
    move = qmp({"execute": "input-send-event", "arguments": {"events": [
        {"type": "rel", "data": {"axis": "x", "value": 200}},
        {"type": "rel", "data": {"axis": "y", "value": 100}}
    ]}})
    click = qmp({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": True}},
        {"type": "btn", "data": {"button": "left", "down": False}}
    ]}})
    return move, click


def send_mouse_rel(dx, dy):
    events = []
    if dx:
        events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy:
        events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    if not events:
        return None
    return qmp({"execute": "input-send-event",
                "arguments": {"events": events}})


def send_mouse_drag(dx, dy, step=24):
    """Sends motion the way a real mouse does: a burst of small packets.

    A single large delta is unreliable in a running game: a PS/2 packet
    carries at most +-127, and an isolated event was observed to leave POP's
    cursor untouched while a rapid burst moved it.  Small successive packets
    also keep the guest's pointer acceleration in its near-linear range.
    """
    remaining_x, remaining_y = int(dx), int(dy)
    while remaining_x or remaining_y:
        hop_x = max(-step, min(step, remaining_x))
        hop_y = max(-step, min(step, remaining_y))
        send_mouse_rel(hop_x, hop_y)
        remaining_x -= hop_x
        remaining_y -= hop_y
        time.sleep(0.02)


def send_mouse_button(down):
    return qmp({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": bool(down)}}
    ]}})


def send_mouse_click(hold=0.9):
    """Presses and releases with a hold long enough for the guest to poll.

    POP samples input once per rendered frame, and the frames arrive slowly
    enough here that an 80 ms press fell between two polls and was never
    seen.
    """
    send_mouse_button(True)
    time.sleep(hold)
    send_mouse_button(False)


# POP's cursor is a dagger whose gold hilt is the only strongly gold cluster
# on its menus (menu text peaks at r+g-2b == 104, the hilt reaches 200), and
# the blade always points up-left from it by this fixed offset, measured on
# two runs at different pointer positions.
CURSOR_GOLD_SCORE = 140
CURSOR_HOTSPOT_DX = -22
CURSOR_HOTSPOT_DY = -29


def locate_dagger_hotspot(path):
    """Returns POP's pointer hotspot from the rendered dagger, or None.

    The guest has a PS/2 mouse only, so QMP can send relative motion but
    cannot address a pixel, and the guest's pointer acceleration makes the
    landing spot of a given delta unpredictable.  POP draws its cursor into
    the frame, so the rendered dagger is an authoritative read-back: locate
    it and the move needed to reach a control is a subtraction.
    """
    frame = read_bmp(path)
    if not frame:
        return None
    w, h, px = frame
    gold = []
    best = CURSOR_GOLD_SCORE
    peak = None
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            score = px[o] + px[o + 1] - 2 * px[o + 2]
            if score < CURSOR_GOLD_SCORE:
                continue
            gold.append((x, y))
            if score > best:
                best = score
                peak = (x, y)
    if peak is None:
        return None
    # Keep only the cluster around the brightest pixel: stray gold from a
    # highlighted control must not drag the centroid off the sprite.
    sx = sy = count = 0
    for x, y in gold:
        if abs(x - peak[0]) <= 24 and abs(y - peak[1]) <= 24:
            sx += x
            sy += y
            count += 1
    if count < 6:
        return None
    return (sx // count + CURSOR_HOTSPOT_DX,
            sy // count + CURSOR_HOTSPOT_DY)


def settle_cursor(path, timeout=16.0, interval=2.0, slack=2):
    """Reads the dagger position once the ring has caught up.

    Ring slots are throttled and their contents trail the guest by seconds,
    so a single read can report where the pointer used to be.  Two agreeing
    reads an interval apart mean the ring has drained.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        if screendump(path):
            found = locate_dagger_hotspot(path)
            if found:
                if (last and abs(found[0] - last[0]) <= slack
                        and abs(found[1] - last[1]) <= slack):
                    return found
                last = found
        time.sleep(interval)
    return last


# Motion measured on the profile screen: the guest amplifies a single large
# packet (100 units moved the pointer 177 px) and attenuates a train of small
# ones, so the loop drives 100-unit packets and seeds this gain.  Each move
# re-measures it, so a wrong seed costs an iteration, never accuracy.
POINTER_GAIN = 1.77
POINTER_STEP = 100


def mouse_move_to(x, y, run_dir, tag, tries=10, tolerance=10):
    """Walks the pointer onto (x, y) using the rendered cursor as feedback."""
    scratch = os.path.join(run_dir, f"cursor-{tag}.bmp")
    seen = settle_cursor(scratch)
    if not seen:
        return None
    gain_x = gain_y = POINTER_GAIN
    for attempt in range(tries):
        dx = x - seen[0]
        dy = y - seen[1]
        if abs(dx) <= tolerance and abs(dy) <= tolerance:
            break
        command_x = max(-900.0, min(900.0, dx / gain_x))
        command_y = max(-900.0, min(900.0, dy / gain_y))
        send_mouse_drag(command_x, command_y, step=POINTER_STEP)
        time.sleep(1.5)
        landed = settle_cursor(scratch)
        if not landed:
            # Off the visible surface: the guest clamps at the corner, so a
            # long negative burst restores a known position.
            send_mouse_drag(-2500, -2500, step=POINTER_STEP)
            landed = settle_cursor(scratch)
            if not landed:
                break
            seen = landed
            continue
        if abs(command_x) > 20 and abs(landed[0] - seen[0]) > 3:
            gain_x = max(0.05, min(5.0,
                                   (landed[0] - seen[0]) / command_x))
        if abs(command_y) > 20 and abs(landed[1] - seen[1]) > 3:
            gain_y = max(0.05, min(5.0,
                                   (landed[1] - seen[1]) / command_y))
        seen = landed
    return seen



def read_bmp(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"BM" or len(data) < 54:
        return None
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if w <= 0 or bpp != 32:
        return None
    flip = h > 0
    h = abs(h)
    px = data[off:off + w * h * 4]
    # Convert BGRA rows (bottom-up when flip) to top-down RGB triplets.
    rgb = bytearray(w * h * 3)
    for y in range(h):
        sy = (h - 1 - y) if flip else y
        base = sy * w * 4
        obase = y * w * 3
        for x in range(w):
            i = base + x * 4
            o = obase + x * 3
            rgb[o] = px[i + 2]
            rgb[o + 1] = px[i + 1]
            rgb[o + 2] = px[i]
    return w, h, bytes(rgb)


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] == b"BM":
        return read_bmp(path)
    if not data.startswith(b"P6"):
        return None
    parts = []
    i = 2
    while len(parts) < 3:
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if i < len(data) and data[i:i + 1] == b"#":
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            continue
        j = i
        while j < len(data) and data[j] not in b" \t\r\n":
            j += 1
        parts.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = parts
    px = data[i:i + w * h * 3]
    return w, h, px


def frame_stats(path):
    parsed = read_ppm(path)
    if not parsed:
        return None
    w, h, px = parsed
    step = max(1, (len(px) // 3) // 20000) * 3
    total = n = 0
    for i in range(0, len(px) - 2, step):
        total += px[i] + px[i + 1] + px[i + 2]
        n += 3
    return {"w": w, "h": h, "mean": total / max(1, n), "step": step}


def frame_delta(a, b):
    pa, pb = read_ppm(a), read_ppm(b)
    if not pa or not pb or pa[:2] != pb[:2]:
        return 255.0
    xa, xb = pa[2], pb[2]
    step = max(1, (len(xa) // 3) // 20000) * 3
    total = n = 0
    for i in range(0, min(len(xa), len(xb)) - 2, step):
        total += abs(xa[i] - xb[i]) + abs(xa[i + 1] - xb[i + 1]) + \
                 abs(xa[i + 2] - xb[i + 2])
        n += 3
    return total / max(1, n)


def looks_pop_title(path):
    """Detect the title logo on its predominantly black background.

    Calibrated against the real SoT render, which is far dimmer than a
    bright-gold assumption: logo pixels sit around (80, 77, 73) with a
    warm ordering r >= g >= b inside the central logo band, over a >70%
    near-black frame.  FMV flesh tones can trip the warm test too, which
    is harmless: a false positive just sends an early skip Enter."""
    parsed = read_ppm(path)
    if not parsed:
        return False
    w, h, px = parsed
    logo = dark = 0
    for y in range(h):
        for x in range(0, w, 2):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            if r < 30 and g < 30 and b < 30:
                dark += 1
            if (150 <= y <= 400 and r > 55 and r >= g >= b and
                    r - b > 4):
                logo += 1
    sampled = h * (w // 2)
    return logo > 1500 and dark > sampled * 0.5


def looks_desktop_ready(path):
    """Detect this image's green XP Start button (Explorer is ready)."""
    parsed = read_ppm(path)
    if not parsed:
        return False
    w, h, px = parsed
    hits = n = 0
    for y in range(max(0, h - 45), h):
        for x in range(min(120, w)):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            if (15 <= r <= 120 and 80 <= g <= 230 and b <= 130 and
                    g > r * 1.3 and g > b * 1.3):
                hits += 1
            n += 1
    return bool(n and hits / n > 0.08)


def looks_desktop(path):
    if looks_desktop_ready(path):
        return True
    parsed = read_ppm(path)
    if not parsed:
        return False
    w, h, px = parsed
    hits = n = 0
    step = max(1, (len(px) // 3) // 8000) * 3
    for i in range(0, len(px) - 2, step):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if 30 <= r <= 90 and 90 <= g <= 140 and 150 <= b <= 220:
            hits += 1
        n += 1
    return bool(n and hits / n > 0.35)


def retroarch_running():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {FRONTEND_IMAGE}"],
                         capture_output=True, text=True).stdout
    return FRONTEND_IMAGE in out


def shutdown(proc):
    # RetroArch does not exit when the guest powers off; wait for the
    # GUEST to halt (query-status), then quit QEMU and kill the frontend
    # promptly instead of hanging on the process. If QMP never started or
    # the core already unloaded, there is no live guest disk to drain.
    if not qmp_alive():
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except (OSError, subprocess.TimeoutExpired):
            subprocess.run(["taskkill", "/IM", FRONTEND_IMAGE, "/F"],
                           capture_output=True)
        return
    halted = False
    try:
        qmp({"execute": "system_powerdown"}, timeout=5)
        for _ in range(40):
            time.sleep(1)
            try:
                st = qmp({"execute": "query-status"}, timeout=5)
                status = st.get("return", {}).get("status", "")
                if status in ("shutdown", "prelaunch", "paused"):
                    halted = True
                    break
            except OSError:
                halted = True
                break
    except OSError:
        pass
    if not halted:
        print("guest did not halt in 40s; forcing")
    try:
        qmp({"execute": "quit"}, timeout=5)
    except OSError:
        pass
    time.sleep(3)
    if retroarch_running():
        subprocess.run(["taskkill", "/IM", FRONTEND_IMAGE, "/F"],
                       capture_output=True)
        time.sleep(2)


def main():
    global QMP_PORT, FRONTEND_IMAGE, CORE
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmdline")
    ap.add_argument("--boot-wait", type=int, default=35,
                    help="minimum wait before polling for Explorer")
    ap.add_argument("--no-launch", action="store_true",
                    help="skip the desktop launch phase (game autostarts)")
    ap.add_argument("--run-command",
                    help="launch this exact command through the Windows Run dialog")
    ap.add_argument("--confirm-left", action="store_true",
                    help="select the left button before confirming the launch dialog")
    ap.add_argument("--load-wait", type=int, default=25)
    ap.add_argument("--skip-secs", type=int, default=170,
                    help="maximum seconds from first Space key to verdict")
    ap.add_argument("--stop-on-title", action="store_true",
                    help="stop Space input on the selected title occurrence")
    ap.add_argument("--title-occurrence", type=int, default=2,
                    help="title-screen transition on which input stops")
    ap.add_argument("--title-settle-secs", type=int, default=10,
                    help="input-free wait after title detection")
    ap.add_argument("--enter-title", action="store_true",
                    help="send one Enter after the input-free title wait")
    ap.add_argument("--menu-mouse", action="store_true",
                    help="move and click the mouse before menu capture")
    ap.add_argument("--menu-wait", type=int, default=0,
                    help="after the title advance, poll up to this many "
                         "seconds for a non-title, non-black frame before "
                         "the verdict window (the title-to-menu fade "
                         "otherwise gets scored as a black menu)")
    ap.add_argument("--verdict-frames", type=int, default=12)
    ap.add_argument("--animation-delta", type=float, default=0.1,
                    help="minimum mean RGB delta for a changed frame")
    ap.add_argument("--label", default="pop-cycle")
    ap.add_argument("--frontend", choices=["retroarch", "retro-host"],
                    default="retroarch",
                    help="retro-host runs the headless frozen frontend")
    ap.add_argument("--retro-host", default=RETRO_HOST,
                    help="retro_host.exe path (default: XPD_RETRO_HOST)")
    ap.add_argument("--retroarch", default=RETROARCH,
                    help="RetroArch path (default: XPD_RETROARCH)")
    ap.add_argument("--core", default=CORE,
                    help="core DLL (default: XPD_CORE, then paired core build-win64)")
    ap.add_argument("--qmp-port", type=int, default=QMP_PORT)
    ap.add_argument("--wait-ms", type=int, default=900000,
                    help="retro_host --wait budget for the whole session")
    ap.add_argument("--start-new-game", action="store_true",
                    help="after the menu verdict, Enter Start Game and probe gameplay")
    ap.add_argument("--game-load-wait", type=int, default=12,
                    help="seconds between Start Game and the cinematic skip")
    ap.add_argument("--game-skip-secs", type=int, default=60,
                    help="Space-spam window for the opening cinematic")
    ap.add_argument("--gameplay-capture-secs", type=int, default=30,
                    help="frame capture window once gameplay should be live")
    ap.add_argument("--script", default="",
                    help="after the menu verdict, drive a literal key script "
                         "and screenshot after every step.  Steps are "
                         "semicolon-separated 'key' or 'key:delay_ms'; a step "
                         "of the form 'text=WORD' types WORD one key at a "
                         "time.  Default delay is 1200 ms.")
    args = ap.parse_args()
    if args.frontend == "retro-host" and not args.retro_host:
        print("--frontend retro-host requires --retro-host or XPD_RETRO_HOST")
        return 4
    if args.frontend == "retroarch" and not args.retroarch:
        print("--frontend retroarch requires --retroarch or XPD_RETROARCH")
        return 4

    QMP_PORT = args.qmp_port
    CORE = args.core
    FRONTEND_IMAGE = ("retro_host.exe" if args.frontend == "retro-host"
                      else "retroarch.exe")

    cmdline = os.path.abspath(args.cmdline) if args.cmdline else None
    if not cmdline:
        cands = sorted(glob.glob(os.path.join(
            WINXP, "xpdriver-xpdm-games-pop-*-test.qemu_cmd_line")))
        if not cands:
            print("no pop test cmdline found")
            return 4
        cmdline = cands[-1]
    version_match = re.search(r"pop-(\d+)-test", os.path.basename(cmdline))
    log_patterns = []
    try:
        with open(cmdline, errors="replace") as fh:
            cmdline_text = fh.read()
        for match in re.finditer(r"-(?:debugcon|serial)\s+file:([^\s]+)",
                                 cmdline_text):
            log_patterns.insert(0, os.path.basename(match.group(1)))
    except OSError:
        pass
    if version_match:
        version = version_match.group(1)
        log_patterns[:0] = ["xpdm-games-pop-%s-debug.log" % version,
                            "xpdm-games-pop-%s-result.log" % version]
    log_patterns = list(dict.fromkeys(log_patterns))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(REPO, "artifacts", "runs",
                           f"{stamp}-{args.label}")
    os.makedirs(run_dir, exist_ok=True)
    print(f"cmdline: {os.path.basename(cmdline)}")
    print(f"run dir: {run_dir}")

    if retroarch_running():
        print("retroarch already running; refusing (disk safety)")
        return 4

    env = dict(os.environ, XPD_FRAME_RING="1")
    for pattern in set(log_patterns + ["frame-ring-*.bmp"]):
        for stale in glob.glob(os.path.join(WINXP, pattern)):
            try:
                os.remove(stale)
            except OSError:
                pass
    frontend_log = open(os.path.join(run_dir, "frontend.log"), "w")
    if args.frontend == "retro-host":
        popen_args = [args.retro_host, args.core, cmdline,
                      str(args.qmp_port), "--wait", str(args.wait_ms),
                      "--no-quit"]
        popen_kwargs = {"creationflags":
                        getattr(subprocess, "CREATE_NO_WINDOW", 0)}
    else:
        popen_args = [args.retroarch, "-L", CORE, cmdline]
        popen_kwargs = {}
    proc = subprocess.Popen(popen_args, cwd=WINXP, env=env,
                            stdout=frontend_log,
                            stderr=subprocess.STDOUT, **popen_kwargs)
    verdict = {"verdict": "ERROR", "detail": "", "frames": []}
    try:
        deadline = time.time() + 180
        while time.time() < deadline and not qmp_alive():
            if proc.poll() is not None:
                print("frontend exited during boot")
                return 4
            time.sleep(3)
        if not qmp_alive():
            print("QMP never came up")
            return 4
        print("QMP up; waiting for Explorer", flush=True)
        time.sleep(args.boot_wait)
        boot_frame = os.path.join(run_dir, "boot-done.bmp")
        desktop_ready = False
        for _ in range(60):
            if screendump(boot_frame) and looks_desktop_ready(boot_frame):
                desktop_ready = True
                break
            time.sleep(2)
        if not desktop_ready:
            verdict.update(verdict="DESKTOP",
                           detail="Explorer did not become ready")
            print("Explorer readiness timeout")
            return 3

        if not args.no_launch:
            print("desktop launch phase", flush=True)
            if args.run_command:
                sendkeys("meta_l", "r")
                time.sleep(1)
                send_text(args.run_command)
                sendkey("ret")
                verdict["launch_command"] = args.run_command
            else:
                # Focus the desktop and select the unique "Play ..." shortcut
                # by incremental keyboard search. This legacy path is suitable
                # only when the image has exactly one matching shortcut.
                sendkeys("meta_l", "d")
                time.sleep(1)
                for key in ("p", "l", "a", "y"):
                    sendkey(key)
                    time.sleep(0.15)
                time.sleep(1)
                sendkey("ret")
            time.sleep(4)
            screendump(os.path.join(run_dir, "launch-dialog-early.bmp"))
            time.sleep(4)
            screendump(os.path.join(run_dir, "launch-dialog.bmp"))
            if args.confirm_left:
                sendkeys("shift", "tab")
                time.sleep(1)
                screendump(os.path.join(
                    run_dir, "launch-dialog-left-selected.bmp"))
            sendkey("ret")
            time.sleep(3)
            screendump(os.path.join(run_dir, "launch-post-answer.bmp"))
            # A crash-recovery ("Use safe mode settings?") or follow-up
            # launcher confirmation can appear after the first answer.  Both
            # keep the affirmative button focused, so extra Enters confirm
            # them; once the game has focus these are harmless skip input.
            for extra in range(2):
                time.sleep(5)
                sendkey("ret")
                screendump(os.path.join(
                    run_dir, f"launch-answer-extra-{extra}.bmp"))
            print(f"waiting {args.load_wait}s for the game to load",
                  flush=True)
            time.sleep(args.load_wait)

        print("spacebar skip phase", flush=True)
        skip_start = time.time()
        end = skip_start + args.skip_secs
        i = 0
        title_active = False
        title_occurrences = 0
        space_input_stopped = False
        initial_title_advance_pending = False
        initial_title_advanced = False
        while time.time() < end:
            captured = False
            skip_frame = os.path.join(
                run_dir, f"skip-{i if args.stop_on_title else i//3:02d}.bmp")
            if args.stop_on_title:
                captured = screendump(skip_frame)
                if captured:
                    is_title = looks_pop_title(skip_frame)
                    if is_title and not title_active:
                        title_occurrences += 1
                        print(f"title occurrence {title_occurrences} detected",
                              flush=True)
                        if title_occurrences == 1:
                            space_input_stopped = True
                            print("waiting for initial title transition",
                                  flush=True)
                    if (not is_title and title_active and
                            title_occurrences == 1 and
                            not initial_title_advanced):
                        initial_title_advance_pending = True
                        initial_title_advanced = True
                        print("advancing first post-title frame once",
                              flush=True)
                    title_active = is_title
                if title_occurrences >= args.title_occurrence:
                    verdict["title_detected_secs"] = round(
                        time.time() - skip_start, 2)
                    verdict["title_occurrence"] = title_occurrences
                    print("stopping Space input", flush=True)
                    time.sleep(args.title_settle_secs)
                    screendump(os.path.join(run_dir, "title-settled.bmp"))
                    if args.enter_title:
                        print("sending one Enter at settled title", flush=True)
                        settled = os.path.join(run_dir, "title-settled.bmp")
                        sendkey("ret")
                        time.sleep(5)
                        post = os.path.join(run_dir, "title-post-enter.bmp")
                        post_ok = screendump(post)
                        # Retry when Enter had no visible effect.  The title
                        # detector alone cannot gate this: it is calibrated
                        # for a bright title and is blind to dim or fading
                        # title states, which would suppress the retries
                        # exactly when they are needed most.
                        unchanged = False
                        if post_ok:
                            delta = frame_delta(settled, post)
                            unchanged = (delta is not None and
                                         delta < 1.0)
                        if post_ok and unchanged:
                            # The title ignored the first Enter.  Try the
                            # plausible focus/input variants, capturing each
                            # outcome so the log shows which one (if any)
                            # the game answers.
                            # Alt+Tab backgrounds the game and forces a D3D
                            # device reset, which turns a missed Enter into a
                            # black screen and a session-churn storm.  Retry
                            # with in-focus keys only.
                            for vname, vkeys in (
                                    ("enter", [("ret",)]),
                                    ("space", [("spc",)]),
                                    ("enter2", [("ret",)])):
                                for vkey in vkeys:
                                    sendkeys(*vkey)
                                    time.sleep(1)
                                time.sleep(6)
                                screendump(os.path.join(
                                    run_dir,
                                    f"title-probe-{vname}.bmp"))
                                print(f"title probe {vname} captured",
                                      flush=True)
                                probe = os.path.join(
                                    run_dir, f"title-probe-{vname}.bmp")
                                pdelta = frame_delta(settled, probe)
                                if (pdelta is not None and
                                        pdelta >= 1.0):
                                    print(f"title answered {vname}",
                                          flush=True)
                                    break
                    break
            if initial_title_advance_pending:
                sendkey("spc")
                initial_title_advance_pending = False
            elif not space_input_stopped:
                # SoT's FMVs respond to Enter at least as reliably as to
                # Space; alternating both keys keeps long intro segments
                # from eating the whole skip window.
                sendkey("ret" if i % 2 else "spc")
            if not args.stop_on_title and i % 3 == 0:
                screendump(skip_frame)
            i += 1
            time.sleep(2)

        if args.menu_wait and args.enter_title:
            print(f"menu wait phase (up to {args.menu_wait}s)", flush=True)
            menu_wait_end = time.time() + args.menu_wait
            menu_seen = False
            while time.time() < menu_wait_end:
                probe = os.path.join(run_dir, "menu-wait.bmp")
                if screendump(probe):
                    stats = frame_stats(probe)
                    if (stats and stats["mean"] >= 6.0 and
                            not looks_pop_title(probe)):
                        print("non-title content visible after "
                              f"{round(time.time() - skip_start, 2)}s",
                              flush=True)
                        menu_seen = True
                        break
                time.sleep(2)
            verdict["menu_wait_seen"] = menu_seen

        if args.menu_mouse:
            move_result, click_result = send_mouse_move_click()
            verdict["mouse_move_result"] = move_result
            verdict["mouse_click_result"] = click_result
            print(f"menu mouse move={move_result} click={click_result}",
                  flush=True)
            time.sleep(3)

        print("verdict phase", flush=True)
        frames = []
        for k in range(args.verdict_frames):
            p = os.path.join(run_dir, f"menu-{k:02d}.bmp")
            if screendump(p):
                frames.append(p)
            time.sleep(1.5)
        stats = [frame_stats(p) for p in frames]
        deltas = [frame_delta(frames[k], frames[k + 1])
                  for k in range(len(frames) - 1)]
        means = [s["mean"] for s in stats if s]
        verdict["frames"] = [os.path.basename(p) for p in frames]
        verdict["means"] = [round(m, 2) for m in means]
        verdict["deltas"] = [round(d, 3) for d in deltas]
        if not means:
            verdict.update(verdict="ERROR", detail="no frames captured")
        elif max(means) < 6.0:
            verdict.update(verdict="BLACK",
                           detail="all verdict frames black")
        elif frames and looks_desktop(frames[-1]):
            verdict.update(verdict="DESKTOP",
                           detail="XP desktop visible; POP not running")
        elif deltas and sum(
                1 for d in deltas if d >= args.animation_delta) >= min(
                    3, len(deltas)):
            verdict.update(verdict="ANIMATING",
                           detail="menu frames differ; presentation alive")
        else:
            verdict.update(verdict="STATIC",
                           detail="non-black frame frozen")
        print(f"VERDICT: {verdict['verdict']} ({verdict['detail']})")
        print(f"means={verdict['means']}")
        print(f"deltas={verdict['deltas']}")

        if args.script:
            print("script phase", flush=True)
            steps = [s.strip() for s in args.script.split(";") if s.strip()]
            script_log = []
            for index, step in enumerate(steps):
                key, _, delay = step.partition(":")
                wait_ms = int(delay) if delay else 1200
                landed = None
                if key.startswith("text="):
                    send_text(key[len("text="):])
                elif key.startswith("click=") or key.startswith("moveto="):
                    name, _, where = key.partition("=")
                    cx, _, cy = where.partition(",")
                    landed = mouse_move_to(int(cx), int(cy), run_dir,
                                           f"{index:02d}")
                    if name == "click":
                        send_mouse_click()
                elif key == "slam":
                    # Pin the pointer at the top-left: the guest clamps
                    # there, turning an unknown position into a known one.
                    send_mouse_drag(-2500, -2500, step=POINTER_STEP)
                elif key == "alttab":
                    sendkeys("alt", "tab")
                elif key.startswith("keys="):
                    sendkeys(*key[len("keys="):].split("+"))
                elif key.startswith("rel="):
                    rx, _, ry = key[len("rel="):].partition(",")
                    send_mouse_rel(int(rx), int(ry))
                elif key.startswith("drag="):
                    rx, _, ry = key[len("drag="):].partition(",")
                    send_mouse_drag(int(rx), int(ry))
                elif key == "click":
                    send_mouse_click()
                elif key != "wait":
                    sendkey(key)
                time.sleep(wait_ms / 1000.0)
                shot = os.path.join(run_dir, f"script-{index:02d}.bmp")
                stats = frame_stats(shot) if screendump(shot) else None
                if landed is None:
                    landed = locate_dagger_hotspot(shot)
                script_log.append({
                    "step": step,
                    "frame": os.path.basename(shot),
                    "cursor": landed,
                    "mean": round(stats["mean"], 2) if stats else None,
                })
                print(f"  {index:02d} {step} -> "
                      f"cursor={landed} mean={script_log[-1]['mean']}",
                      flush=True)
            verdict["script"] = script_log

        if args.start_new_game:
            print("start-new-game phase", flush=True)
            sendkey("ret")  # main menu default selection: Start Game
            time.sleep(args.game_load_wait)
            print("opening-cinematic skip phase", flush=True)
            game_skip_end = time.time() + args.game_skip_secs
            gi = 0
            while time.time() < game_skip_end:
                sendkey("spc")
                if gi % 3 == 0:
                    screendump(os.path.join(
                        run_dir, f"game-skip-{gi//3:02d}.bmp"))
                gi += 1
                time.sleep(2)
            print("gameplay capture phase", flush=True)
            gframes = []
            capture_end = time.time() + args.gameplay_capture_secs
            k = 0
            while time.time() < capture_end:
                p = os.path.join(run_dir, f"gameplay-{k:02d}.bmp")
                if screendump(p):
                    gframes.append(p)
                k += 1
                time.sleep(1.5)
            gmeans = [s["mean"] for s in
                      (frame_stats(p) for p in gframes) if s]
            gdeltas = [frame_delta(gframes[j], gframes[j + 1])
                       for j in range(len(gframes) - 1)]
            verdict["gameplay_frames"] = [os.path.basename(p)
                                          for p in gframes]
            verdict["gameplay_means"] = [round(m, 2) for m in gmeans]
            verdict["gameplay_deltas"] = [round(d, 3) for d in gdeltas]
            if not gmeans:
                verdict["gameplay_verdict"] = "ERROR"
                verdict["gameplay_detail"] = "no gameplay frames captured"
            elif max(gmeans) < 6.0:
                verdict["gameplay_verdict"] = "GAMEPLAY_BLACK"
                verdict["gameplay_detail"] = "all gameplay frames black"
            elif gdeltas and sum(
                    1 for d in gdeltas if d >= args.animation_delta) >= min(
                        3, len(gdeltas)):
                verdict["gameplay_verdict"] = "GAMEPLAY_ANIMATING"
                verdict["gameplay_detail"] = (
                    "gameplay frames differ; presentation alive")
            else:
                verdict["gameplay_verdict"] = "GAMEPLAY_STATIC"
                verdict["gameplay_detail"] = "non-black gameplay frame frozen"
            print(f"GAMEPLAY VERDICT: {verdict['gameplay_verdict']} "
                  f"({verdict['gameplay_detail']})")
            print(f"gameplay means={verdict['gameplay_means']}")
            print(f"gameplay deltas={verdict['gameplay_deltas']}")
    finally:
        shutdown(proc)
        try:
            frontend_log.close()
        except OSError:
            pass
        for pat in log_patterns:
            for src in glob.glob(os.path.join(WINXP, pat)):
                try:
                    dst = os.path.join(run_dir, os.path.basename(src))
                    with open(src, "rb") as a, open(dst, "wb") as b:
                        b.write(a.read())
                except OSError:
                    pass
        with open(os.path.join(run_dir, "verdict.json"), "w") as fh:
            json.dump(verdict, fh, indent=2)
    gameplay = verdict.get("gameplay_verdict")
    if gameplay:
        return {"GAMEPLAY_ANIMATING": 0, "GAMEPLAY_BLACK": 2,
                "GAMEPLAY_STATIC": 2}.get(gameplay, 4)
    return {"ANIMATING": 0, "STATIC": 2, "BLACK": 2,
            "DESKTOP": 3}.get(verdict["verdict"], 4)


if __name__ == "__main__":
    sys.exit(main())
