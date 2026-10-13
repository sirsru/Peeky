import curses
import sys
import termios

P_TEXT, P_ACCENT, P_PANEL, P_MUTED = 1, 2, 3, 4

SETTINGS = {
    "search_engine": "duckduckgo",
    "render_type": "text as text",
    "block_images": False,
    "block_scripts": False,
}

RENDER_OPTIONS = ["text as text", "all as grid"]
SELECTABLE_KEYS = [
    ("search_engine", "Search engine"),
    ("render_type", "Render type"),
    ("block_images", "Block images"),
    ("block_scripts", "Block scripts"),
]


def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(P_TEXT, curses.COLOR_WHITE, -1)
    curses.init_pair(P_ACCENT, curses.COLOR_GREEN, -1)
    curses.init_pair(P_PANEL, curses.COLOR_WHITE, curses.COLOR_BLACK)
    curses.init_pair(P_MUTED, curses.COLOR_WHITE, -1)


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


def draw_settings(stdscr):
    init_colors()
    stdscr.bkgd(" ", attr(P_TEXT))
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    stdscr.timeout(80)

    sel = 0
    while True:
        stdscr.erase()
        h, w = stdscr.getmaxyx()
        box_w = max(min(w - 4, 58), 10)
        bx = max((w - box_w) // 2, 0)
        box_h = 3 + len(SELECTABLE_KEYS) * 2 + 2
        by = max((h - box_h) // 2, 0)

        draw_box(stdscr, bx, by, box_w, box_h)
        put(stdscr, by + 2, bx + 4, "SETTINGS", attr(P_ACCENT, curses.A_BOLD))

        for i, (key, label) in enumerate(SELECTABLE_KEYS):
            y = by + 4 + i * 2
            val = SETTINGS[key]
            if key == "render_type":
                val_str = val
            elif isinstance(val, bool):
                val_str = "on" if val else "off"
            else:
                val_str = str(val)
            a = attr(P_ACCENT, curses.A_BOLD) if sel == i else attr(P_TEXT)
            put(stdscr, y, bx + 4, f"{label}: {val_str}", a)

        put(stdscr, h - 1, 2, "tab: move  enter: edit  ctrl+q: quit", mut())
        stdscr.refresh()

        key = stdscr.getch()
        if key in (3, 17):
            break
        elif key == 9:
            sel = (sel + 1) % len(SELECTABLE_KEYS)
        elif key == 353:
            sel = (sel - 1) % len(SELECTABLE_KEYS)
        elif key in (10, 13, curses.KEY_ENTER):
            key_name, label = SELECTABLE_KEYS[sel]
            if key_name == "render_type":
                idx = RENDER_OPTIONS.index(SETTINGS["render_type"])
                SETTINGS["render_type"] = RENDER_OPTIONS[(idx + 1) % len(RENDER_OPTIONS)]
            elif isinstance(SETTINGS[key_name], bool):
                SETTINGS[key_name] = not SETTINGS[key_name]
            else:
                result = read_input(stdscr, bx, by + len(SELECTABLE_KEYS) + 6, box_w, first=str(SETTINGS[key_name]), highlight=True)
                if result != "":
                    SETTINGS[key_name] = result


def main():
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
