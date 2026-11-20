#!/usr/bin/env python3
# ---------------------------------------------------------------------------
#  Scarecrow Terminal  --  Anubis Darkwatch
#  A keyboard-driven console for the Evil Crow "Scarecrow" RX-only collector,
#  built for the M5Stack Cardputer Zero (Raspberry Pi CM0 / Linux).
#
#  RECEIVE-ONLY. This app only reads the collector's web endpoints and sends
#  tune/label/calibrate commands the firmware already exposes. No transmit
#  path exists anywhere in Scarecrow.
#
#  Zero third-party dependencies: Python 3 stdlib (curses, urllib, json) so it
#  packages as a tiny .deb and runs on a stock Raspberry Pi OS image.
# ---------------------------------------------------------------------------
import curses, json, time, threading, urllib.request, urllib.parse, os, sys

COLLECTOR = os.environ.get("SCARECROW_HOST", "http://192.168.4.1")
POLL_SEC  = 1.0
HTTP_TO   = 1.5
BANDS     = [315.00, 433.92, 868.30, 915.00]

# ---- shared state (written by poller, read by UI) -------------------------
S = {
    "nodes": [], "target": None, "sigs": [], "chans": [],
    "log": {"ok": False, "bytes": 0, "cap": 1, "full": False},
    "online": False, "last_ok": 0.0, "err": "",
}
LOCK = threading.Lock()
STOP = threading.Event()


def _get(path):
    try:
        with urllib.request.urlopen(COLLECTOR + path, timeout=HTTP_TO) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:                      # noqa: BLE001  (offline is normal)
        raise e


def _cmd(path):
    """Fire-and-forget GET (tune / label / calibrate / forget)."""
    try:
        with urllib.request.urlopen(COLLECTOR + path, timeout=HTTP_TO) as r:
            r.read()
        return True
    except Exception:
        return False


def poller():
    while not STOP.is_set():
        try:
            nodes = _get("/nodes.json")
            sigs  = _get("/sigs.json")
            state = _get("/state.json")
            try:    logj = _get("/log.json")
            except Exception:    logj = None
            with LOCK:
                S["nodes"]  = nodes.get("nodes", [])
                S["target"] = nodes.get("target")
                S["sigs"]   = sigs.get("sigs", [])
                S["chans"]  = state.get("channels", [])
                if logj:    S["log"] = logj
                S["online"] = True
                S["last_ok"] = time.time()
                S["err"] = ""
        except Exception as e:
            with LOCK:
                S["online"] = False
                S["err"] = type(e).__name__
        STOP.wait(POLL_SEC)


# ---- color -----------------------------------------------------------------
C = {}
def init_colors():
    curses.start_color()
    try: curses.use_default_colors()
    except curses.error: pass
    # pair id : (fg, bg)
    defs = {
        "ink":   (curses.COLOR_WHITE,  -1),
        "gold":  (curses.COLOR_YELLOW, -1),
        "teal":  (curses.COLOR_CYAN,   -1),
        "hot":   (curses.COLOR_RED,    -1),
        "dim":   (curses.COLOR_BLUE,   -1),
        "ok":    (curses.COLOR_GREEN,  -1),
        "sel":   (curses.COLOR_BLACK,  curses.COLOR_YELLOW),
        "selt":  (curses.COLOR_BLACK,  curses.COLOR_CYAN),
    }
    for i, (name, (fg, bg)) in enumerate(defs.items(), start=1):
        try:
            curses.init_pair(i, fg, bg)
            C[name] = curses.color_pair(i)
        except curses.error:
            C[name] = curses.A_NORMAL


def heat(rssi):
    """RSSI -> color: strong=crimson, mid=gold, weak=teal."""
    if rssi is None: return C["dim"]
    if rssi >= -55:  return C["hot"]
    if rssi >= -80:  return C["gold"]
    return C["teal"]


def safe_add(w, y, x, s, attr=0):
    h, wd = w.getmaxyx()
    if y < 0 or y >= h: return
    s = s[: max(0, wd - x - 1)]
    try: w.addstr(y, x, s, attr)
    except curses.error: pass


# ---- views -----------------------------------------------------------------
VIEWS = ["LOCATE", "SIGNALS", "MONITOR", "CONTROL"]


def draw_header(w, view_idx):
    h, wd = w.getmaxyx()
    safe_add(w, 0, 0, "SCARECROW", C["gold"] | curses.A_BOLD)
    safe_add(w, 0, 10, "· anubis darkwatch", C["dim"])
    with LOCK:
        online, tgt, err = S["online"], S["target"], S["err"]
    stat = f"{tgt:.2f} MHz" if tgt else "----"
    dot = "● LIVE" if online else ("○ " + (err or "OFFLINE"))
    safe_add(w, 0, wd - len(dot) - 1, dot, (C["ok"] if online else C["hot"]))
    safe_add(w, 1, 0, stat, C["teal"])
    # tab strip
    x = 0
    for i, v in enumerate(VIEWS):
        tag = f" {v} "
        attr = (C["sel"] | curses.A_BOLD) if i == view_idx else C["dim"]
        safe_add(w, 2, x, tag, attr)
        x += len(tag) + 1
    safe_add(w, 3, 0, "─" * (wd - 1), C["dim"])


def view_locate(w, top):
    with LOCK:
        nodes = list(S["nodes"]); tgt = S["target"]
    rovers = [n for n in nodes if not n.get("self") and n.get("age", 99) < 12]
    if rovers:
        safe_add(w, top, 0, f"{len(rovers)} rover(s) · hottest = nearest", C["dim"])
    else:
        safe_add(w, top, 0, "Standalone · anchor fox-meter", C["dim"])
    nodes = sorted(nodes, key=lambda n: n.get("rssi", -200), reverse=True)
    h, wd = w.getmaxyx()
    y = top + 1
    for n in nodes:
        if y >= h - 1: break
        rssi = n.get("rssi"); nid = n.get("id", "?")
        tag = "EVIL CROW" if n.get("self") else f"NODE {nid}"
        age = n.get("age", 0)
        stale = "" if age < 12 else f" ({age}s)"
        line = f"{tag:<10} {rssi:>4} dBm{stale}"
        safe_add(w, y, 0, line, heat(rssi) | (curses.A_BOLD if y == top + 1 else 0))
        # bar: map -110..-40 -> 0..barw
        barw = max(6, wd - len(line) - 2)
        filled = 0 if rssi is None else int(max(0, min(1, (rssi + 110) / 70.0)) * barw)
        safe_add(w, y, len(line) + 1, "█" * filled, heat(rssi))
        y += 1


def view_signals(w, top, sel, scroll):
    with LOCK:
        sigs = list(S["sigs"])
    sigs = sorted(sigs, key=lambda s: s.get("age", 99))   # freshest first
    h, wd = w.getmaxyx()
    if not sigs:
        safe_add(w, top, 0, "No signatures yet.", C["dim"])
        return sigs
    safe_add(w, top, 0, "↑↓ pick · L label · D forget", C["dim"])
    y = top + 1
    view_rows = h - 1 - y
    for i in range(scroll, min(len(sigs), scroll + view_rows)):
        s = sigs[i]
        name = s.get("label") or f"#{s.get('id')}"
        meta = f"{s.get('rssi'):>4}dBm {s.get('count')}x {s.get('age')}s"
        row = f"{name:<16} {meta}"
        attr = (C["sel"] | curses.A_BOLD) if i == sel else heat(s.get("rssi"))
        safe_add(w, y, 0, (">" if i == sel else " ") + row, attr)
        y += 1
    return sigs


def view_monitor(w, top):
    with LOCK:
        chans = list(S["chans"])
    h, wd = w.getmaxyx()
    safe_add(w, top, 0, f"{'MHz':<8}{'RSSI':>6}{'busy':>6}{'floor':>7}", C["dim"])
    y = top + 1
    for c in chans:
        if y >= h - 1: break
        mhz = c.get("mhz", "?"); rssi = c.get("rssi", -127)
        line = f"{mhz:<8}{rssi:>6}{c.get('busy',0):>6}{c.get('floor',-110):>7}"
        safe_add(w, y, 0, line, heat(rssi))
        y += 1


def view_control(w, top, band_idx):
    with LOCK:
        tgt = S["target"]; log = dict(S["log"])
    safe_add(w, top, 0, "Mesh band — ←→ pick · ENTER set", C["dim"])
    y = top + 1
    x = 0
    h, wd = w.getmaxyx()
    for i, b in enumerate(BANDS):
        tag = f" {b:.2f} "
        on = (tgt is not None and abs(b - tgt) < 0.05)
        if i == band_idx:
            attr = C["sel"] | curses.A_BOLD
        elif on:
            attr = C["ok"] | curses.A_BOLD
        else:
            attr = C["dim"]
        if x + len(tag) >= wd - 1:
            x = 0; y += 1
        safe_add(w, y, x, tag, attr)
        x += len(tag) + 1
    y += 2
    safe_add(w, y, 0, f"Now watching: {tgt:.2f} MHz" if tgt else "Now watching: ----", C["teal"])
    if log.get("ok"):
        kb = log.get("bytes", 0) / 1024.0
        cap = log.get("cap", 1) / 1024.0
        msg = f"Log {kb:.1f}/{cap:.0f} KB" + ("  FULL" if log.get("full") else "")
        safe_add(w, y + 1, 0, msg + "   (G=download via web)", C["gold"] if log.get("full") else C["dim"])


# ---- text input (label a signature) ---------------------------------------
def prompt(w, label):
    h, wd = w.getmaxyx()
    curses.curs_set(1)
    buf = ""
    while True:
        safe_add(w, h - 1, 0, " " * (wd - 1))
        safe_add(w, h - 1, 0, f"{label}: {buf}", C["gold"])
        w.refresh()
        ch = w.getch()
        if ch in (curses.KEY_ENTER, 10, 13):
            break
        if ch == 27:                       # ESC cancels
            buf = None; break
        if ch in (curses.KEY_BACKSPACE, 127, 8):
            buf = buf[:-1]
        elif 32 <= ch < 127 and len(buf) < 17:
            buf += chr(ch)
    curses.curs_set(0)
    return buf


# ---- main loop -------------------------------------------------------------
def main(stdscr):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(200)
    init_colors()

    view = 0          # 0 locate 1 signals 2 monitor 3 control
    sel = 0           # signature cursor
    scroll = 0
    band_idx = 0
    toast = ""
    toast_until = 0

    threading.Thread(target=poller, daemon=True).start()

    while True:
        stdscr.erase()
        h, wd = stdscr.getmaxyx()
        draw_header(stdscr, view)
        body_top = 4
        if view == 0:
            view_locate(stdscr, body_top)
        elif view == 1:
            sigs = view_signals(stdscr, body_top, sel, scroll)
        elif view == 2:
            view_monitor(stdscr, body_top)
        else:
            view_control(stdscr, body_top, band_idx)

        if toast and time.time() < toast_until:
            safe_add(stdscr, h - 1, 0, toast.ljust(wd - 1), C["sel"])
        elif view != 1:
            safe_add(stdscr, h - 1, 0, "TAB views · Q quit", C["dim"])
        stdscr.refresh()

        ch = stdscr.getch()
        if ch == -1:
            continue
        if ch in (ord("q"), ord("Q")):
            break
        if ch == ord("\t") or ch == curses.KEY_RIGHT and view != 3:
            view = (view + 1) % len(VIEWS); sel = 0; scroll = 0; continue
        if ch == curses.KEY_BTAB:
            view = (view - 1) % len(VIEWS); continue
        if ch in (ord("1"), ord("2"), ord("3"), ord("4")):
            view = ch - ord("1"); continue

        if view == 1:                                  # SIGNALS
            with LOCK: n = len(S["sigs"])
            if ch == curses.KEY_DOWN and sel < n - 1: sel += 1
            elif ch == curses.KEY_UP and sel > 0: sel -= 1
            elif ch in (ord("l"), ord("L")) and n:
                with LOCK: cur = sorted(S["sigs"], key=lambda s: s.get("age",99))
                if sel < len(cur):
                    sid = cur[sel].get("id")
                    name = prompt(stdscr, "label")
                    if name:
                        ok = _cmd("/label?id=%d&name=%s" % (sid, urllib.parse.quote(name)))
                        toast = "labeled" if ok else "send failed"
                        toast_until = time.time() + 1.5
            elif ch in (ord("d"), ord("D")) and n:
                with LOCK: cur = sorted(S["sigs"], key=lambda s: s.get("age",99))
                if sel < len(cur):
                    sid = cur[sel].get("id")
                    ok = _cmd("/forget?id=%d" % sid)
                    toast = "forgotten" if ok else "send failed"
                    toast_until = time.time() + 1.5
            # keep cursor on screen
            vis = h - 6
            if sel < scroll: scroll = sel
            if sel >= scroll + vis: scroll = sel - vis + 1

        elif view == 3:                                # CONTROL
            if ch == curses.KEY_LEFT: band_idx = (band_idx - 1) % len(BANDS)
            elif ch == curses.KEY_RIGHT: band_idx = (band_idx + 1) % len(BANDS)
            elif ch in (curses.KEY_ENTER, 10, 13):
                ok = _cmd("/fox?mhz=%.2f" % BANDS[band_idx])
                toast = ("tuned %.2f" % BANDS[band_idx]) if ok else "send failed"
                toast_until = time.time() + 1.5

    STOP.set()


def run():
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print("Scarecrow Terminal — Anubis Darkwatch")
        print("Usage: scarecrow-term   (set SCARECROW_HOST to override %s)" % COLLECTOR)
        sys.exit(0)
    run()
