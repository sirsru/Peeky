import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "peeky-engine"))

VERBOSE = "--verbose" in sys.argv

import re
import curses
import locale
import random
import termios
import time
import textwrap
from urllib.parse import quote

import fetch
import settings
from paint import render_to_grid, draw_grid, draw_scrollbar, log_error, set_verbose

LOG_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "log.txt")

if VERBOSE:
    set_verbose(True)
    sys.argv.remove("--verbose")

WORD = "peeky"
STRIDE = 6                              # 5 + 1 space
BANNER_W = STRIDE * len(WORD) - 1       # 29
BANNER_H = 7

FONT = {
    "p": ["█████",
          "█   █",
          "█   █",
          "█   █",
          "█████",
          "█    ",
          "█    "],
    "e": [" ███ ",
          "█   █",
          "█████",
          "█    ",
          " ███ "],
    "k": ["█   █",
          "█  █ ",
          "███  ",
          "█  █ ",
          "█   █"],
    "y": ["█   █",
          " █ █ ",
          "  █  ",
          "  █  ",
          "  █  ",
          "  █  ",
          " ██  "],
}

EYE_CLOSED = ["     ",
              "     ",
              "█████",
              "     ",
              "     "]

HINT = "type to search | ctrl+q to quit"
SHORTCUTS = "ctrl+t/w tab | ctrl+n/p switch | ctrl+l url"
FOOTER_HOME = "↑↓/j/k select | enter open | type to search | ctrl+q quit"
FOOTER_ERROR = "esc: home | ctrl+q: quit"

BLINK_SHUT = 0.14
BLINK_GAP = (2.6, 5.2)
DOUBLE_BLINK_P = 0.18

P_TEXT, P_ACCENT, P_PANEL, P_MUTED = 1, 2, 3, 4
P_TAB_ACTIVE, P_TAB_INACTIVE = 5, 6

SELECTABLES = ["search", "github", "settings"]

URL_RE = re.compile(r"^[\w-]+(\.[\w-]+)+(:\d+)?([/?#]\S*)?$")
TITLE_RE = re.compile(rb"<title[^>]*>(.*?)</title>", re.I | re.S)

# Tab strip
TAB_STRIP_H = 2          # row 0 labels, row 1 rule
TAB_MIN_W = 10
TAB_MAX_W = 26
TAB_PLUS_W = 3

TAB_HITS = []   # (x0, x1, kind, index), half-open, rebuilt every draw
HOME_HITS = []  # (x0, x1, y, kind, sel-index), rebuilt every draw

WHEEL_UP = curses.BUTTON4_PRESSED | curses.BUTTON4_CLICKED
WHEEL_DOWN = (getattr(curses, "BUTTON5_PRESSED", 0) |
              getattr(curses, "BUTTON5_CLICKED", 0))
LEFT_CLICK = curses.BUTTON1_PRESSED | curses.BUTTON1_CLICKED

SPINNER = "|/-\\"


def page_rows(h):
    return h - TAB_STRIP_H - 1


def tab_hit(mx):
    for x0, x1, kind, i in TAB_HITS:
        if x0 <= mx < x1:
            return kind, i
    return None, -1


def home_hit(mx, my):
    for x0, x1, y, kind, i in HOME_HITS:
        if y == my and x0 <= mx < x1:
            return kind, i
    return None, -1


def new_tab(tabs):
    tabs.append(Tab())
    return len(tabs) - 1


def close_tab(tabs, i):
    if len(tabs) == 1:
        tabs[0] = Tab()
        return 0
    tabs.pop(i)
    return min(i, len(tabs) - 1)


class Tab:
    def __init__(self):
        self.mode = "home"            # "home" | "page" | "error"
        self.title = "new tab"
        self.url = ""
        self.html = b""
        self.base_dir = None
        self.page = None
        self.message = ""
        self.size = None              # (cols, rows) the grid was rendered for
        self.palette = {}
        self.grid = []
        self.scroll = 0


def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(P_TEXT, curses.COLOR_WHITE, -1)
    curses.init_pair(P_ACCENT, curses.COLOR_GREEN, -1)
    curses.init_pair(P_PANEL, curses.COLOR_WHITE, curses.COLOR_BLACK)
    curses.init_pair(P_MUTED, curses.COLOR_WHITE, -1)
    curses.init_pair(P_TAB_ACTIVE, curses.COLOR_GREEN, -1)
    curses.init_pair(P_TAB_INACTIVE, curses.COLOR_WHITE, -1)


def attr(pair, extra=0):
    return curses.color_pair(pair) | extra


def mut():
    return attr(P_MUTED, curses.A_DIM)


def put(scr, y, x, s, a):
    h, w = scr.getmaxyx()
    if 0 <= y < h and 0 <= x and x + len(s) <= w:
        try:
            scr.addstr(y, x, s, a)
        except curses.error:
            pass


def draw_panel(scr, px, py, panel_w, panel_h):
    b = mut()
    for dy in range(panel_h):
        put(scr, py + dy, px, " " * panel_w, attr(P_PANEL))
    scr.hline(py, px, curses.ACS_HLINE | b, panel_w)
    scr.hline(py + panel_h - 1, px, curses.ACS_HLINE | b, panel_w)
    scr.vline(py, px, curses.ACS_VLINE | b, panel_h)
    scr.vline(py, px + panel_w - 1, curses.ACS_VLINE | b, panel_h)
    scr.addch(py, px, curses.ACS_ULCORNER | b)
    scr.addch(py, px + panel_w - 1, curses.ACS_URCORNER | b)
    scr.addch(py + panel_h - 1, px, curses.ACS_LLCORNER | b)
    scr.addch(py + panel_h - 1, px + panel_w - 1, curses.ACS_LRCORNER | b)


def draw_tab_strip(scr, tabs, active, w):
    act = attr(P_TAB_ACTIVE, curses.A_BOLD)
    ina = attr(P_TAB_INACTIVE, curses.A_DIM)
    rule = mut()
    n = len(tabs)

    hits = []

    put(scr, 0, 0, " " * w, ina)
    put(scr, 1, 0, "─" * w, rule)

    avail = max(w - TAB_PLUS_W - 2, 4)
    tab_w = min(max(avail // n, TAB_MIN_W), TAB_MAX_W, avail)
    visible = max(1, avail // tab_w)
    start = 0 if n <= visible else max(0, min(active - visible // 2, n - visible))
    end = min(n, start + visible)

    put(scr, 0, 0, "‹" if start > 0 else " ", rule)
    x = 1
    for i in range(start, end):
        is_act = i == active
        inner = tab_w - 1
        room = max(inner - 2 - (2 if is_act else 0), 0)
        label = tabs[i].title
        if len(label) > room:
            label = label[:max(room - 1, 0)] + ("…" if room else "")
        text = " " + label.ljust(room) + (" ×" if is_act else "") + " "
        if is_act:
            hits.append((x + room + 1, x + room + 4, "close", i))
        hits.append((x, x + inner, "tab", i))
        put(scr, 0, x, text, act if is_act else ina)
        if is_act:
            put(scr, 1, x, "━" * inner, act)
        elif i + 1 != active and i + 1 < end:
            put(scr, 0, x + inner, "│", rule)
        x += tab_w
    put(scr, 0, x, "›" if end < n else " ", rule)
    put(scr, 0, x + 1, " + ", attr(P_ACCENT))
    hits.append((x, x + 4, "new", -1))
    TAB_HITS[:] = hits


def draw_banner(scr, y, x, eyes_open):
    h, w = scr.getmaxyx()
    eye_a = attr(P_ACCENT, curses.A_BOLD)
    text_a = attr(P_TEXT, curses.A_BOLD)
    for i, ch in enumerate(WORD):
        rows = FONT[ch]
        if ch == "e" and not eyes_open:
            rows = EYE_CLOSED
        a = eye_a if ch == "e" else text_a
        for r, row in enumerate(rows):
            for c, cell in enumerate(row):
                if cell != "█":
                    continue
                yy, xx = y + r, x + i * STRIDE + c
                if 0 <= yy < h and 0 <= xx < w:
                    try:
                        scr.addch(yy, xx, curses.ACS_BLOCK | a)
                    except curses.error:
                        pass


def draw_box(scr, bx, by, box_w, box_h=3, highlight=False):
    fill = attr(P_ACCENT, curses.A_BOLD) if highlight else attr(P_TEXT)
    b = attr(P_ACCENT, curses.A_BOLD) if highlight else mut()
    for dy in range(box_h):
        put(scr, by + dy, bx, " " * box_w, fill)
    scr.hline(by, bx, curses.ACS_HLINE | b, box_w)
    scr.hline(by + box_h - 1, bx, curses.ACS_HLINE | b, box_w)
    scr.vline(by, bx, curses.ACS_VLINE | b, box_h)
    scr.vline(by, bx + box_w - 1, curses.ACS_VLINE | b, box_h)
    scr.addch(by, bx, curses.ACS_ULCORNER | b)
    scr.addch(by, bx + box_w - 1, curses.ACS_URCORNER | b)
    scr.addch(by + box_h - 1, bx, curses.ACS_LLCORNER | b)
    scr.addch(by + box_h - 1, bx + box_w - 1, curses.ACS_LRCORNER | b)
    hint_attr = attr(P_ACCENT, curses.A_BOLD) if highlight else mut()
    put(scr, by + 1, bx + 2, "> ", hint_attr)
    put(scr, by + 1, bx + 4, "search or type a url", hint_attr)


def scroll_key(tab, key, view, instant):
    max_s = max(0, len(tab.grid) - view)
    page = max(1, view - 1)
    step = {curses.KEY_UP: -1, ord("k"): -1, curses.KEY_DOWN: 1, ord("j"): 1,
            curses.KEY_PPAGE: -page, ord("b"): -page,
            curses.KEY_NPAGE: page, ord(" "): page}.get(key)
    if step is not None:
        if instant:
            step = view if step > 0 else -view
        new = tab.scroll + step
    elif key in (curses.KEY_HOME, ord("g")):
        new = 0
    elif key in (curses.KEY_END, ord("G")):
        new = max_s
    else:
        return False
    tab.scroll = max(0, min(max_s, new))
    return True


def draw_screen(scr, eyes_open, sel=0):
    h, w = scr.getmaxyx()
    use_banner = w >= BANNER_W + 2 and h >= BANNER_H + 7
    if use_banner:
        ly = max((h - (BANNER_H + 2 + 3 + 2 + 3)) // 2, 0)
        draw_banner(scr, ly, (w - BANNER_W) // 2, eyes_open)
        by = ly + BANNER_H + 2
    else:
        ly = max((h - 7) // 2, 0)
        put(scr, ly, (w - len(WORD)) // 2, WORD, attr(P_ACCENT, curses.A_BOLD))
        by = ly + 3

    box_w = max(min(w - 4, 58), 10)
    bx = max((w - box_w) // 2, 0)
    can_box = by + 2 <= h - 2 and box_w >= 8
    hits = []
    if can_box:
        highlight_box = sel == 0
        draw_box(scr, bx, by, box_w, highlight=highlight_box)
        hint_a = mut() if not highlight_box else attr(P_ACCENT, curses.A_BOLD)
        put(scr, by + 4, bx + (box_w - len(HINT)) // 2, HINT, hint_a)
        hits.append((bx, bx + box_w, by, "search", 0))

        for idx, label in ((1, "github"), (2, "settings")):
            y = by + 4 + idx
            if y >= h - 1:
                break
            text = f"› {label}" if sel == idx else f"  {label}"
            a = attr(P_ACCENT, curses.A_BOLD) if sel == idx else mut()
            x = bx + (box_w - len(text)) // 2
            put(scr, y, x, text, a)
            hits.append((x, x + len(text), y, "menu", idx))

        if h > by + 8:
            put(scr, by + 7, bx + (box_w - len(SHORTCUTS)) // 2, SHORTCUTS, mut())
    HOME_HITS[:] = hits
    scr.refresh()
    return can_box, bx, by, box_w


def draw_footer(scr, tab, w, h):
    if h < 1:
        return
    if tab.mode == "page":
        rows = page_rows(h)
        max_s = max(0, len(tab.grid) - rows)
        pct = 100 if max_s == 0 else int(min(100.0, tab.scroll / max_s * 100))
        right = f"{pct}%  esc: home  r: reload  ctrl+q: quit"
        room = max(w - len(right) - 6, 0)
        left = (tab.title or tab.url or "")[:room]
        put(scr, h - 1, 2, left, mut())
        put(scr, h - 1, max(w - len(right) - 2, len(left) + 3), right, mut())
    elif tab.mode == "error":
        put(scr, h - 1, 2, FOOTER_ERROR, mut())
    else:
        put(scr, h - 1, max((w - len(FOOTER_HOME)) // 2, 0), FOOTER_HOME, mut())


def read_input(scr, bx, by, box_w, first="", highlight=False):
    buf = first
    max_w = box_w - 6
    while True:
        if buf:
            lines = [buf[i:i + max_w] for i in range(0, len(buf), max_w)]
        else:
            lines = [""]
        content_lines = max(1, len(lines))
        box_h = min(7, max(3, content_lines + 2))

        # erase old hint drawn by draw_screen at fixed by+4
        put(scr, by + 4, bx, " " * box_w, attr(P_TEXT))

        draw_box(scr, bx, by, box_w, box_h, highlight=highlight)
        for i, line in enumerate(lines):
            put(scr, by + 1 + i, bx + 4, line.ljust(max_w), attr(P_ACCENT, curses.A_BOLD) if highlight else attr(P_TEXT))
        put(scr, by + box_h + 1, bx + (box_w - len(HINT)) // 2, HINT, attr(P_ACCENT, curses.A_BOLD) if highlight else mut())

        last_line = lines[-1]
        cursor_y = by + 1 + (len(lines) - 1)
        cursor_x = bx + 4 + min(len(last_line), max_w)
        try:
            scr.move(cursor_y, cursor_x)
        except curses.error:
            pass
        scr.refresh()

        k = scr.getch()
        if k in (10, 13, curses.KEY_ENTER):
            return buf.strip()
        if k == 27:  # esc key
            return ""
        if k in (curses.KEY_BACKSPACE, 127, 8):
            buf = buf[:-1]
        elif 32 <= k < 127 and len(buf) < max_w * 5:
            buf += chr(k)


def resolve_query(q):
    if q.startswith(("http://", "https://")):
        return q, None
    if q.startswith("file://"):
        return q, os.path.dirname(q[7:]) or "."
    if not any(c.isspace() for c in q) and URL_RE.match(q):
        return "https://" + q, None
    return "https://www.google.com/search?q=" + quote(q), None


def page_title(html, fallback):
    m = TITLE_RE.search(html)
    if m:
        t = " ".join(m.group(1).decode("utf-8", "replace").split())
        if t:
            return t
    return fallback


def navigate(stdscr, tab, q):
    h, w = stdscr.getmaxyx()
    stdscr.clear()
    stdscr.refresh()

    state = {"name": "", "t0": 0.0, "spin": 0}

    def status(name, elapsed=None):
        if elapsed is None:
            state["name"], state["t0"] = name, time.monotonic()
            state["spin"] = (state["spin"] + 1) % len(SPINNER)
            text = f"{SPINNER[state['spin']]} {name}…"
        else:
            text = f"{name} {elapsed:.1f}s"
        put(stdscr, h - 1, 0, " " * w, attr(P_PANEL))
        put(stdscr, h - 1, 1, text, attr(P_ACCENT, curses.A_BOLD))
        stdscr.refresh()

    status("fetch", None)
    url, base_dir = resolve_query(q)
    try:
        load = fetch.fetch_page_async(
            url, on_status=status,
            cache_size=settings.SETTINGS["cache_size"],
            workers=settings.SETTINGS["workers"],
        )
    except Exception as e:
        log_error(f"fetch_page failed for {url}", e)
        tab.mode, tab.title, tab.message, tab.size = "error", "error", str(e), None
        return
    html = load.html
    if isinstance(html, str):
        html = html.encode("utf-8", "replace")
    fallback = (url.split("://", 1)[-1].split("/", 1)[0]
                if url.startswith("http") else os.path.basename(url))
    tab.mode, tab.html, tab.base_dir, tab.size = "page", html, base_dir, None
    tab.url = url
    tab.title = page_title(html, fallback)
    tab.scroll = 0
    tab.page = load

    rows = page_rows(h)
    status("render", None)
    tab.palette, tab.grid = render_to_grid(tab.html, w, rows, tab.base_dir,
                                           on_status=status, page=load)
    tab.size = (w, rows)


def draw_page(scr, tab, w, h):
    top, rows = TAB_STRIP_H, page_rows(h)
    if tab.mode == "error":
        msg_lines = textwrap.wrap(tab.message, max(w - 8, 10))[:max(rows - 6, 1)]
        panel_w = min(max(max((len(l) for l in msg_lines), default=0) + 8, 30), w - 4)
        panel_h = min(len(msg_lines) + 6, max(h - 2, 5))
        px = max((w - panel_w) // 2, 0)
        py = max((h - panel_h) // 2, 0)
        draw_panel(scr, px, py, panel_w, panel_h)
        put(scr, py + 1, px + 2, "couldn't load this page", attr(P_ACCENT, curses.A_BOLD))
        for i, line in enumerate(msg_lines):
            put(scr, py + 3 + i, px + 2, line, mut())
        scr.refresh()
        return
    if rows < 2 or w < 2:
        return
    if tab.size != (w, rows):
        tab.palette, tab.grid = render_to_grid(tab.html, w, rows, tab.base_dir, page=tab.page)
        tab.size = (w, rows)
    total = len(tab.grid)
    max_s = max(0, total - rows)
    tab.scroll = max(0, min(tab.scroll, max_s))
    if tab.grid:
        # leave the last column for the scrollbar when one is shown
        limit = w - 1 if max_s else w
        draw_scrollbar(scr, top, rows, total, tab.scroll, w - 1)
        scr.refresh()                              # chrome + scrollbar to screen
        draw_grid(scr, tab.palette, tab.grid, top, tab.scroll, rows, limit=limit)  # raw ANSI truecolor grid
    else:
        put(scr, top + 1, 2, "render failed - see log.txt", mut())
        scr.refresh()


def prompt_line(scr, label="go: "):
    h, w = scr.getmaxyx()
    scr.timeout(-1)
    scr.clear()
    scr.refresh()
    buf = ""
    while True:
        put(scr, h - 1, 0, " " * w, attr(P_PANEL))
        text = (label + buf)[-(w - 3):]
        put(scr, h - 1, 1, text, attr(P_ACCENT, curses.A_BOLD))
        try:
            scr.move(h - 1, min(1 + len(text), w - 1))
        except curses.error:
            pass
        k = scr.getch()
        if k in (10, 13, curses.KEY_ENTER):
            return buf.strip()
        if k == 27:
            return ""
        if k in (curses.KEY_BACKSPACE, 127, 8):
            buf = buf[:-1]
        elif 32 <= k < 127:
            buf += chr(k)


def draw_home(stdscr):
    init_colors()
    curses.mouseinterval(0)
    curses.mousemask(curses.ALL_MOUSE_EVENTS)
    stdscr.bkgd(" ", attr(P_TEXT))
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    tabs, active = [Tab()], 0
    eyes_open = True
    next_blink = time.monotonic() + random.uniform(*BLINK_GAP)
    shut_until = 0.0
    sel = 0

    while True:
        try:
            tab = tabs[active]
            h, w = stdscr.getmaxyx()
            can_box, bx, by, box_w = False, 0, 0, 0

            if tab.mode == "home":
                stdscr.timeout(80)
                now = time.monotonic()
                if eyes_open and now >= next_blink:
                    eyes_open, shut_until = False, now + BLINK_SHUT
                elif not eyes_open and now >= shut_until:
                    eyes_open = True
                    gap = 0.30 if random.random() < DOUBLE_BLINK_P else random.uniform(*BLINK_GAP)
                    next_blink = now + gap
            else:
                stdscr.timeout(-1)

            stdscr.clear()
            draw_tab_strip(stdscr, tabs, active, w)
            draw_footer(stdscr, tab, w, h)
            if tab.mode == "home":
                can_box, bx, by, box_w = draw_screen(stdscr, eyes_open, sel)
            else:
                draw_page(stdscr, tab, w, h)

            key = stdscr.getch()
            if key == -1 or key == curses.KEY_RESIZE:
                continue
            if key in (3, 17):                       # ctrl+c / ctrl+q
                break
            elif key == curses.KEY_MOUSE:
                try:
                    _id, mx, my, _z, bstate = curses.getmouse()
                except curses.error:
                    continue
                if my < TAB_STRIP_H:
                    if bstate & LEFT_CLICK:
                        kind, i = tab_hit(mx)
                        if kind == "new":
                            active, sel = new_tab(tabs), 0
                        elif kind == "close":
                            active = close_tab(tabs, i)
                        elif kind == "tab":
                            active = i
                elif bstate & LEFT_CLICK and tab.mode == "home":
                    kind, i = home_hit(mx, my)
                    if kind == "search" and can_box:
                        sel = 0
                        q = read_input(stdscr, bx, by, box_w, highlight=True)
                        if q:
                            navigate(stdscr, tab, q)
                    elif kind == "menu":
                        sel = i
                        if i == 1:
                            navigate(stdscr, tab, "github.com")
                        elif i == 2:
                            settings.draw_settings(stdscr)
                elif tab.mode == "page":
                    view = page_rows(h)
                    max_s = max(0, len(tab.grid) - view)
                    instant = settings.SETTINGS.get("scrolling") == "instant"
                    jump = view if instant else 3
                    if bstate & WHEEL_UP:
                        tab.scroll = max(0, tab.scroll - jump)
                    elif bstate & WHEEL_DOWN:
                        tab.scroll = min(max_s, tab.scroll + jump)
            elif key == 20:                          # ctrl+t: new tab
                tabs.append(Tab())
                active, sel = len(tabs) - 1, 0
            elif key == 23:                          # ctrl+w: close tab
                if len(tabs) == 1:
                    tabs[0] = Tab()
                else:
                    tabs.pop(active)
                    active = min(active, len(tabs) - 1)
            elif key == 16:                          # ctrl+p: next tab
                active = (active + 1) % len(tabs)
            elif key == 14:                          # ctrl+n: previous tab
                active = (active - 1) % len(tabs)
            elif key == 12:                          # ctrl+l: address bar
                if tab.mode == "home":
                    sel = 0
                    if can_box:
                        q = read_input(stdscr, bx, by, box_w, highlight=True)
                        if q:
                            navigate(stdscr, tab, q)
                else:
                    q = prompt_line(stdscr)
                    if q:
                        navigate(stdscr, tab, q)
            elif tab.mode == "page":
                view = page_rows(h)
                if key == 27:                        # esc: back to home
                    tab.mode = "home"
                elif key == ord("r") and tab.url:    # reload
                    navigate(stdscr, tab, tab.url)
                elif ord("1") <= key <= ord("9"):    # jump to tab n
                    idx = key - ord("1")
                    if idx < len(tabs):
                        active = idx
                else:
                    scroll_key(tab, key, view,
                               settings.SETTINGS.get("scrolling") == "instant")
            elif tab.mode == "home":
                if key in (9, 353, curses.KEY_DOWN, curses.KEY_UP,
                           ord("j"), ord("k")):
                    step = -1 if key in (353, curses.KEY_UP, ord("k")) else 1
                    sel = (sel + step) % len(SELECTABLES)
                elif key in (10, 13, curses.KEY_ENTER):
                    if sel == 1:
                        navigate(stdscr, tab, "github.com")
                    elif sel == 2:
                        settings.draw_settings(stdscr)
                    elif sel == 0 and can_box:
                        q = read_input(stdscr, bx, by, box_w, highlight=True)
                        if q:
                            navigate(stdscr, tab, q)
                elif 32 <= key < 127:
                    sel = 0
                    if can_box:
                        q = read_input(stdscr, bx, by, box_w, highlight=True, first=chr(key))
                        if q:
                            navigate(stdscr, tab, q)
            # any other key on a page tab is ignored: the page stays put
        except Exception as e:
            log_error("draw_home loop exception", e)
            try:
                t = tabs[active]
                t.mode, t.title, t.message = "error", "error", str(e)
            except Exception:
                pass


def main():
    try:
        locale.setlocale(locale.LC_ALL, "")
    except locale.Error:
        pass
    os.environ["ESCDELAY"] = "25"
    fd = sys.stdin.fileno()
    old_attrs = None
    try:
        old_attrs = termios.tcgetattr(fd)
        new_attrs = termios.tcgetattr(fd)
        new_attrs[0] &= ~(termios.IXON | termios.IXOFF)
        termios.tcsetattr(fd, termios.TCSANOW, new_attrs)
    except (termios.error, OSError):
        pass
    try:
        curses.wrapper(draw_home)
    except KeyboardInterrupt:
        pass
    except Exception as e:
        log_error("main exception", e)
    finally:
        if old_attrs is not None:
            try:
                termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
            except (termios.error, OSError):
                pass


if __name__ == "__main__":
    main() 