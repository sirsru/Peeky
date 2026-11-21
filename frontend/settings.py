import curses
import locale
import os
import sys
import termios

P_TEXT, P_ACCENT, P_PANEL, P_MUTED = 1, 2, 3, 4
P_VALUE, P_HEADER = 5, 6

SETTINGS = {
    "search_engine": "duckduckgo",
    "render_type": "text as text",
    "block_images": False,
    "block_scripts": False,
    "scrolling": "smoothed",
    "cache_size": 8 * 1024 * 1024,   # 8MB default
    "workers": 8,
}

RENDER_OPTIONS = ["text as text", "all as grid"]
SCROLL_OPTIONS = ["smoothed", "instant"]
SEARCH_ENGINES = ["duckduckgo", "google", "bing", "startpage"]

SLIDERS = {
    "cache_size": (
        [8 * 1024 * 1024, 16 * 1024 * 1024, 32 * 1024 * 1024,
         64 * 1024 * 1024, 128 * 1024 * 1024, 256 * 1024 * 1024,
         512 * 1024 * 1024, 1024 * 1024 * 1024],
        lambda v: f"{v // (1024 * 1024)}MB" if v < 1024 ** 3 else f"{v // 1024 ** 3}GB",
    ),
    "workers": (
        [1, 2, 4, 8, 16, 32],
        lambda v: str(v),
    ),
}

GROUPS = [
    ("General", [
        ("search_engine", "Search engine", "choice", SEARCH_ENGINES),
        ("render_type", "Render type", "choice", RENDER_OPTIONS),
        ("scrolling", "Scrolling", "choice", SCROLL_OPTIONS),
    ]),
    ("Content", [
        ("block_images", "Block images", "toggle", None),
        ("block_scripts", "Block scripts", "toggle", None),
    ]),
    ("Performance", [
        ("cache_size", "Cache size", "slider", None),
        ("workers", "Workers", "slider", None),
    ]),
]

BAR_W = 24


def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(P_TEXT, curses.COLOR_WHITE, -1)
    curses.init_pair(P_ACCENT, curses.COLOR_GREEN, -1)
    curses.init_pair(P_PANEL, curses.COLOR_WHITE, curses.COLOR_BLACK)
    curses.init_pair(P_MUTED, curses.COLOR_WHITE, -1)
    curses.init_pair(P_VALUE, curses.COLOR_CYAN, -1)
    curses.init_pair(P_HEADER, curses.COLOR_YELLOW, -1)


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


def draw_panel(scr, bx, by, panel_w, panel_h):
    b = mut()
    put(scr, by, bx, " " * panel_w, attr(P_PANEL))
    for dy in range(1, panel_h - 1):
        put(scr, by + dy, bx, " ", attr(P_PANEL))
        put(scr, by + dy, bx, "│", b)
        put(scr, by + dy, bx + panel_w - 1, "│", b)
    put(scr, by + panel_h - 1, bx, " " * panel_w, attr(P_PANEL))
    scr.hline(by, bx, curses.ACS_HLINE | b, panel_w)
    scr.hline(by + panel_h - 1, bx, curses.ACS_HLINE | b, panel_w)
    scr.addch(by, bx, curses.ACS_ULCORNER | b)
    scr.addch(by, bx + panel_w - 1, curses.ACS_URCORNER | b)
    scr.addch(by + panel_h - 1, bx, curses.ACS_LLCORNER | b)
    scr.addch(by + panel_h - 1, bx + panel_w - 1, curses.ACS_LRCORNER | b)


def draw_box(scr, bx, by, box_w, box_h=3, highlight=False):
    fill = attr(P_ACCENT, curses.A_BOLD) if highlight else attr(P_TEXT)
    b = attr(P_ACCENT, curses.A_BOLD) if highlight else mut()
    for dy in range(box_h):
        put(scr, by + dy, bx, " " * box_w, fill)
    scr.hline(by, bx, curses.ACS_HLINE | b, box_w)
    scr.hline(by + box_h - 1, bx, curses.ACS_HLINE | b, box_w)
    scr.vline(by, bx, curses.ACS_VLINE | box_h)
    scr.vline(by, bx + box_w - 1, curses.ACS_VLINE | b)
    scr.addch(by, bx, curses.ACS_ULCORNER | b)
    scr.addch(by, bx + box_w - 1, curses.ACS_URCORNER | b)
    scr.addch(by + box_h - 1, bx, curses.ACS_LLCORNER | b)
    scr.addch(by + box_h - 1, bx + box_w - 1, curses.ACS_LRCORNER | b)
    put(scr, by + 1, bx + 2, "> ", attr(P_ACCENT, curses.A_BOLD) if highlight else mut())
    put(scr, by + 1, bx + 4, "search or type a url", mut() if not highlight else attr(P_ACCENT, curses.A_BOLD))


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
        put(scr, by + 4, bx, " " * box_w, attr(P_TEXT))
        draw_box(scr, bx, by, box_w, box_h, highlight=highlight)
        for i, line in enumerate(lines):
            put(scr, by + 1 + i, bx + 4, line.ljust(max_w), attr(P_ACCENT, curses.A_BOLD) if highlight else attr(P_TEXT))
        put(scr, by + box_h + 1, bx + (box_w - len("search or type a url")) // 2, "search or type a url", attr(P_ACCENT, curses.A_BOLD) if highlight else mut())
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
        if k == 27:
            return ""
        if k in (curses.KEY_BACKSPACE, 127, 8):
            buf = buf[:-1]
        elif 32 <= k < 127 and len(buf) < max_w * 5:
            buf += chr(k)


def flatten_items():
    items = []
    for section, rows in GROUPS:
        for key, label, kind, options in rows:
            items.append({"section": section, "key": key, "label": label,
                          "kind": kind, "options": options})
    return items


def slider_index(key):
    values, _ = SLIDERS[key]
    try:
        return values.index(SETTINGS[key])
    except ValueError:
        return min(range(len(values)), key=lambda i: abs(values[i] - SETTINGS[key]))


def adjust(item, step):
    key, kind = item["key"], item["kind"]
    if kind == "toggle":
        SETTINGS[key] = not SETTINGS[key]
    elif kind == "choice":
        opts = item["options"]
        try:
            idx = opts.index(SETTINGS[key])
        except ValueError:
            idx = 0
        SETTINGS[key] = opts[(idx + step) % len(opts)]
    elif kind == "slider":
        values, _ = SLIDERS[key]
        idx = max(0, min(len(values) - 1, slider_index(key) + step))
        SETTINGS[key] = values[idx]


def draw_slider(scr, y, vx, key, selected):
    values, fmt = SLIDERS[key]
    idx = slider_index(key)
    pos = round(idx / (len(values) - 1) * (BAR_W - 1))
    filled = "━" * pos + "●"
    empty = "─" * (BAR_W - 1 - pos)
    val = fmt(SETTINGS[key]).rjust(max(SLIDER_VAL_W.values()))
    if selected:
        put(scr, y, vx, filled, attr(P_ACCENT, curses.A_BOLD))
        put(scr, y, vx + pos + 1, empty, mut())
        put(scr, y, vx + BAR_W + 2, val, attr(P_ACCENT, curses.A_BOLD))
    else:
        put(scr, y, vx, filled + empty, mut())
        put(scr, y, vx + BAR_W + 2, val, mut())
    put(scr, y + 1, vx, fmt(values[0]), mut())
    put(scr, y + 1, vx + BAR_W - len(fmt(values[-1])), fmt(values[-1]), mut())


def draw_choice(scr, y, vx, key, options, geom, selected):
    side_w, cur_w = geom
    try:
        idx = options.index(SETTINGS[key])
    except ValueError:
        idx = 0
    cur = options[idx]
    prev = options[(idx - 1) % len(options)]
    nxt = options[(idx + 1) % len(options)]
    cur_a = attr(P_ACCENT, curses.A_BOLD) if selected else mut()
    put(scr, y, vx, prev.rjust(side_w) + " ", mut())
    put(scr, y, vx + side_w + 1, f"‹ {cur.center(cur_w - 4)} ›", cur_a)
    put(scr, y, vx + side_w + 1 + cur_w, " " + nxt.ljust(side_w), mut())


def draw_toggle(scr, y, vx, key, selected):
    s = "● on" if SETTINGS[key] else "○ off"
    a = attr(P_ACCENT, curses.A_BOLD) if selected else mut()
    put(scr, y, vx, s, a)


SLIDER_VAL_W = {k: max(len(f(v)) for v in vals) for k, (vals, f) in SLIDERS.items()}


def draw_settings(stdscr):
    init_colors()
    stdscr.bkgd(" ", attr(P_TEXT))
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    stdscr.timeout(80)

    items = flatten_items()
    label_w = max(len(it["label"]) for it in items)
    carousel = {
        it["key"]: (max(len(o) for o in it["options"]),
                    max(len(o) for o in it["options"]) + 4)
        for it in items if it["kind"] == "choice"
    }
    slider_w = BAR_W + 2 + max(SLIDER_VAL_W.values())
    choice_w = {k: 2 * s + 1 + c + 1 + s for k, (s, c) in carousel.items()}
    col_w = max([slider_w, 5] + list(choice_w.values()))

    panel_w = max(30, min(8 + label_w + col_w, 66))
    sel = 0

    while True:
        stdscr.erase()
        h, w = stdscr.getmaxyx()

        content_lines = sum(
            3 + sum(2 if r[2] == "slider" else 1 for r in rows)
            for _, rows in GROUPS)
        panel_h = content_lines + 3
        bx = max((w - panel_w) // 2, 0)
        by = max((h - panel_h) // 2, 0)
        compact = by + panel_h > h

        if compact:
            put(stdscr, 0, bx, "SETTINGS", attr(P_ACCENT, curses.A_BOLD))
            y = 1
        else:
            draw_panel(stdscr, bx, by, panel_w, panel_h)
            put(stdscr, by, bx + 2, " SETTINGS ", attr(P_ACCENT, curses.A_BOLD))
            y = by + 2
        row = 0
        for section, rows in GROUPS:
            put(stdscr, y, bx + 2, section.upper(), attr(P_HEADER, curses.A_BOLD))
            put(stdscr, y, bx + 2 + len(section) + 1, "─" * (panel_w - len(section) - 5), mut())
            y += 1 if compact else 2
            for rowt in rows:
                it = {"key": rowt[0], "label": rowt[1],
                      "kind": rowt[2], "options": rowt[3]}
                is_sel = row == sel
                marker = "›" if is_sel else " "
                put(stdscr, y, bx + 2, marker,
                    attr(P_ACCENT, curses.A_BOLD) if is_sel else mut())
                put(stdscr, y, bx + 4, it["label"].ljust(label_w),
                    attr(P_TEXT, curses.A_BOLD) if is_sel else mut())
                vx = bx + 4 + label_w + 2
                if it["kind"] == "slider":
                    draw_slider(stdscr, y, vx, it["key"], is_sel)
                    y += 1
                elif it["kind"] == "choice":
                    draw_choice(stdscr, y, vx, it["key"], it["options"],
                                carousel[it["key"]], is_sel)
                else:
                    draw_toggle(stdscr, y, vx, it["key"], is_sel)
                y += 1
                row += 1
            y += 0 if compact else 1

        put(stdscr, h - 1, 2,
            "↑↓/j/k: move  ←→/enter: change  esc: back", mut())
        stdscr.refresh()

        key = stdscr.getch()
        if key in (27, 3, ord("q")):
            break
        elif key in (curses.KEY_DOWN, 9, ord("j")):
            sel = (sel + 1) % len(items)
        elif key in (curses.KEY_UP, 353, ord("k")):
            sel = (sel - 1) % len(items)
        elif key == curses.KEY_HOME:
            sel = 0
        elif key == curses.KEY_END:
            sel = len(items) - 1
        elif key in (curses.KEY_LEFT, curses.KEY_RIGHT, 10, 13, curses.KEY_ENTER, ord(" ")):
            adjust(items[sel], 1 if key != curses.KEY_LEFT else -1)


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
        new_attrs[3] &= ~(termios.IXON | termios.IXOFF)
        termios.tcsetattr(fd, termios.TCSANOW, new_attrs)
    except (termios.error, OSError):
        pass
    try:
        curses.wrapper(lambda scr: draw_settings(scr))
    except KeyboardInterrupt:
        pass
    finally:
        if old_attrs is not None:
            try:
                termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
            except (termios.error, OSError):
                pass


if __name__ == "__main__":
    main()
