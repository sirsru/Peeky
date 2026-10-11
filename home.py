import curses
import random
import re
import sys
import termios
import time
import webbrowser
from urllib.parse import quote

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

BLINK_SHUT = 0.14
BLINK_GAP = (2.6, 5.2)
DOUBLE_BLINK_P = 0.18

P_TEXT, P_ACCENT, P_PANEL, P_MUTED = 1, 2, 3, 4

URL_RE = re.compile(r"^[\w-]+(\.[\w-]+)+(:\d+)?([/?#]\S*)?$")


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


def draw_box(scr, bx, by, box_w, box_h=3):
    for dy in range(box_h):
        put(scr, by + dy, bx, " " * box_w, attr(P_TEXT))
    b = mut()
    scr.hline(by, bx, curses.ACS_HLINE | b, box_w)
    scr.hline(by + box_h - 1, bx, curses.ACS_HLINE | b, box_w)
    scr.vline(by, bx, curses.ACS_VLINE | b, box_h)
    scr.vline(by, bx + box_w - 1, curses.ACS_VLINE | b, box_h)
    scr.addch(by, bx, curses.ACS_ULCORNER | b)
    scr.addch(by, bx + box_w - 1, curses.ACS_URCORNER | b)
    scr.addch(by + box_h - 1, bx, curses.ACS_LLCORNER | b)
    scr.addch(by + box_h - 1, bx + box_w - 1, curses.ACS_LRCORNER | b)
    put(scr, by + 1, bx + 2, "> ", mut())
    put(scr, by + 1, bx + 4, "search or type a url", mut())


def draw_screen(scr, eyes_open):
    h, w = scr.getmaxyx()
    use_banner = w >= BANNER_W + 2 and h >= BANNER_H + 7
    if use_banner:
        ly = max((h - (BANNER_H + 2 + 3 + 2)) // 2, 0)  # banner+gap+box+hint
        draw_banner(scr, ly, (w - BANNER_W) // 2, eyes_open)
        by = ly + BANNER_H + 2
    else:
        ly = max((h - 7) // 2, 0)
        put(scr, ly, (w - len(WORD)) // 2, WORD, attr(P_ACCENT, curses.A_BOLD))
        by = ly + 3

    box_w = max(min(w - 4, 58), 10)
    bx = max((w - box_w) // 2, 0)
    can_box = by + 2 <= h - 2 and box_w >= 8
    if can_box:
        draw_box(scr, bx, by, box_w)
        put(scr, by + 4, bx + (box_w - len(HINT)) // 2, HINT, mut())

    if h >= 2 and w >= 24:
        put(scr, h - 1, 2, "f1 github", attr(P_MUTED))
        put(scr, h - 1, max(w - 13, 14), "f2 settings", attr(P_MUTED))
    scr.refresh()
    return can_box, bx, by, box_w


def read_input(scr, bx, by, box_w, first=""):
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

        draw_box(scr, bx, by, box_w, box_h)
        for i, line in enumerate(lines):
            put(scr, by + 1 + i, bx + 4, line.ljust(max_w), attr(P_TEXT))
        put(scr, by + box_h + 1, bx + (box_w - len(HINT)) // 2, HINT, mut())

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


def open_query(q):
    if q.startswith(("http://", "https://")):
        url = q
    elif not any(c.isspace() for c in q) and URL_RE.match(q):
        url = "https://" + q
    else:
        url = "https://www.google.com/search?q=" + quote(q)
    webbrowser.open(url)


def draw_home(stdscr):
    init_colors()
    stdscr.bkgd(" ", attr(P_TEXT))
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    stdscr.timeout(80)  # polls inputs?

    eyes_open = True
    next_blink = time.monotonic() + random.uniform(*BLINK_GAP)
    shut_until = 0.0

    while True:
        now = time.monotonic()
        if eyes_open and now >= next_blink:
            eyes_open, shut_until = False, now + BLINK_SHUT
        elif not eyes_open and now >= shut_until:
            eyes_open = True
            gap = 0.30 if random.random() < DOUBLE_BLINK_P else random.uniform(*BLINK_GAP)
            next_blink = now + gap

        stdscr.erase()
        can_box, bx, by, box_w = draw_screen(stdscr, eyes_open)

        key = stdscr.getch()
        if key in (3, 17):  # ctrl+c, ctrl+q
            break
        elif key == curses.KEY_F0 + 1:
            webbrowser.open("https://github.com")
        elif key == curses.KEY_F0 + 2:
            pass  # settings
        elif can_box and key in (curses.KEY_ENTER, 10, 13):
            q = read_input(stdscr, bx, by, box_w)
            if q:
                open_query(q)
        elif can_box and 32 <= key < 127:
            q = read_input(stdscr, bx, by, box_w, first=chr(key))
            if q:
                open_query(q)


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
        curses.wrapper(draw_home)
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
