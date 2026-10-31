import io
import locale
import os
import re
import subprocess
import sys
import tempfile
import time
import traceback
from datetime import datetime

import curses

P_TEXT, P_ACCENT, P_PANEL, P_MUTED = 1, 2, 3, 4

STD_XTERM = {0:(0,0,0),1:(128,0,0),2:(0,128,0),3:(128,128,0),4:(0,0,128),
         5:(128,0,128),6:(0,128,128),7:(192,192,192),8:(128,128,128),
         9:(255,0,0),10:(0,255,0),11:(255,255,0),12:(0,0,255),
         13:(255,0,255),14:(0,255,255),15:(255,255,255)}

_PAIRS = {}
_next_pair = [10]

def _ansi_cell(fg, bg):
    return "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm" % (fg[0],fg[1],fg[2], bg[0],bg[1],bg[2])

def draw_grid(stdscr, palette, grid, top=0, offset=0, height=None, limit=None):
    max_y, max_x = stdscr.getmaxyx()
    if height is None:
        height = max_y - top
    if limit is None:
        limit = max_x
    out = []
    for ri in range(height):
        gi = offset + ri
        y = top + ri
        if gi >= len(grid) or y >= max_y:
            break
        out.append("\x1b[%d;1H" % (y + 1))
        row = grid[gi]
        for x in range(min(limit, len(row))):
            glyph, fg_id, bg_id = row[x]
            fg = palette.get(fg_id, STD_XTERM.get(fg_id, (255, 255, 255)))
            bg = palette.get(bg_id, STD_XTERM.get(bg_id, (0, 0, 0)))
            out.append(_ansi_cell(fg, bg) + glyph)
        out.append("\x1b[0m")
    os.write(1, "".join(out).encode("utf-8", "replace"))

def pair_for(fg, bg, palette=None):
    key = (fg, bg)
    p = _PAIRS.get(key)
    if p is None:
        if _next_pair[0] >= curses.COLOR_PAIRS:
            fg_num, bg_num = fg, bg
        if palette is not None:
            for num in (fg, bg):
                if num in palette:
                    r, g, b = palette[num]
                    try:
                        curses.init_color(num, r * 1000 // 255, g * 1000 // 255, b * 1000 // 255)
                    except curses.error:
                        pass
        try:
            curses.init_pair(_next_pair[0], fg_num, bg_num)
        except curses.error:
            return curses.color_pair(0)
        p = _PAIRS[key] = _next_pair[0]
        _next_pair[0] += 1
    return curses.color_pair(p)

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


PIPELINE_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "peeky-engine")
LOG_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "log.txt")

VERBOSE = False


def log_error(msg, exc=None): 
    with open(LOG_FILE, "a") as f:
        f.write(f"[{datetime.now().isoformat()}] {msg}\n")
        if exc:
            f.write(f"{''.join(traceback.format_exception(type(exc), exc, exc.__traceback__))}\n")
        f.write("\n")


def log_verbose(msg):
    if VERBOSE:
        with open(LOG_FILE, "a") as f:
            f.write(f"[{datetime.now().isoformat()}] [VERBOSE] {msg}\n")


def set_verbose(v):
    global VERBOSE
    VERBOSE = v


def _run_stage(prog, stdin_data=None, args=None):
    cmd = [os.path.join(PIPELINE_DIR, prog)] + (args or [])
    log_verbose(f"Running stage: {prog} {' '.join(args or [])}")
    try:
        r = subprocess.run(cmd, input=stdin_data, capture_output=True, timeout=30)
        if VERBOSE:
            if prog in ("tokenize", "tree", "cssom", "render-tree", "layout"):
                log_verbose(f"Stage {prog} stdout ({len(r.stdout)} bytes):\n{r.stdout.decode()[:5000]}")
            else:
                log_verbose(f"Stage {prog} stdout ({len(r.stdout)} bytes): <binary>")
            if r.stderr:
                log_verbose(f"Stage {prog} stderr:\n{r.stderr.decode()}")
        if r.returncode != 0:
            log_error(f"Stage {prog} failed (exit {r.returncode})", None)
            if r.stderr:
                with open(LOG_FILE, "a") as f:
                    f.write(f"stderr:\n{r.stderr.decode()}\n")
        return r.stdout
    except Exception as e:
        log_error(f"Stage {prog} exception", e)
        return b""


def _extract_css(html, base_dir=None):
    css_parts = []
    for m in re.finditer(rb'<style[^>]*>(.*?)</style>', html, re.DOTALL | re.IGNORECASE):
        css_parts.append(m.group(1))
    for m in re.finditer(rb'<link[^>]+rel=["\']stylesheet["\'][^>]*href=["\']([^"\']+)["\']', html, re.IGNORECASE):
        href = m.group(1).decode()
        if base_dir and not href.startswith(("http://", "https://", "data:")):
            path = os.path.join(base_dir, href)
            if os.path.exists(path):
                with open(path, "rb") as f:
                    css_parts.append(f.read())
                    log_verbose(f"Loaded linked stylesheet: {href}")
            else:
                log_verbose(f"Linked stylesheet not found: {path}")
    return b"\n".join(css_parts)


def run_pipeline(html_bytes, viewport_w=80, viewport_h=24, base_dir=None, term_cols=None, term_rows=None,
                 on_status=None):
    t0 = time.monotonic()

    def tick(name):
        nonlocal t0
        if on_status:
            on_status(name, time.monotonic() - t0)
        t0 = time.monotonic()

    try:
        on_status("tokenize", None)
        #tokenizing
        tok = _run_stage("tokenize", stdin_data=html_bytes)
        tick("tokenize")
        if not tok:
            return {}, 0, 0, []
        on_status("tree", None)
        #tree
        dom = _run_stage("tree", stdin_data=tok)
        tick("tree")
        if not dom:
            return {}, 0, 0, []
        #cssom
        css_raw = _extract_css(html_bytes, base_dir)
        on_status("cssom", None)
        cssom = _run_stage("cssom", stdin_data=css_raw) if css_raw.strip() else b"#stylesheet\n"
        tick("cssom")
        if not cssom:
            return {}, 0, 0, []
        #render-tree
        with tempfile.NamedTemporaryFile(delete=False, suffix=".cssom") as cf:
            cf.write(cssom)
            cssom_path = cf.name
        with tempfile.NamedTemporaryFile(delete=False, suffix=".dom") as df:
            df.write(dom)
            dom_path = df.name
        try:
            on_status("render-tree", None)
            rt = _run_stage("render-tree", args=[cssom_path, dom_path])
        finally:
            os.unlink(cssom_path)
            os.unlink(dom_path)
        tick("render-tree")
        if not rt:
            return {}, 0, 0, []
        #layout
        browser_w = 1280
        if term_cols is None:
            term_cols = viewport_w // 2
        if term_rows is None:
            term_rows = viewport_h // 2
        on_status("layout", None)
        layout = _run_stage("layout", args=[str(browser_w), str(term_cols), str(term_rows)], stdin_data=rt)
        tick("layout")
        if not layout:
            return {}, 0, 0, []
        #paint
        on_status("paint", None)
        paint_args = []
        if base_dir:
            paint_args.extend(["--base-dir", base_dir])
        paint = _run_stage("paint", args=paint_args, stdin_data=layout)
        tick("paint")
        if not paint:
            return {}, 0, 0, []
        return parse_dump(io.StringIO(paint.decode("utf-8", "replace")))
    except Exception as e:
        log_error("run_pipeline exception", e)
        return {}, 0, 0, []


def render_to_grid(html_bytes, cols, rows, base_dir=None, on_status=None):
    palette, _c, _r, grid = run_pipeline(html_bytes, cols * 2, rows * 2,
                                         base_dir, cols, rows, on_status)
    return palette, grid


def draw_scrollbar(scr, top, view, total, offset, x):
    thumb = max(1, view * view // total)
    pos = offset * (view - thumb) // max(1, total - view)
    for i in range(view):
        on = pos <= i < pos + thumb
        put(scr, top + i, x, "┃" if on else "│",
            attr(P_ACCENT, curses.A_BOLD) if on else mut())


def render_page(html_bytes, stdscr, viewport_w=80, viewport_h=24, base_dir=None):
    max_y, max_x = stdscr.getmaxyx()
    palette, grid = render_to_grid(html_bytes, max_x, max_y, base_dir)
    stdscr.clear()
    draw_grid(stdscr, palette, grid)
    stdscr.getch()


def parse_dump(fp):
    palette = {}
    cols = 0
    rows = 0
    grid = []

    state = None
    for line in fp:
        line = line.rstrip("\n")
        if not line:
            continue
        if line.startswith("#palette"):
            state = "palette"
            continue
        if line.startswith("#paint"):
            parts = line.split()
            cols = int(parts[1])
            rows = int(parts[2])
            state = "paint"
            continue
        if state == "palette":
            parts = line.split()
            if len(parts) >= 4:
                pid = int(parts[0])
                r, g, b = int(parts[1]), int(parts[2]), int(parts[3])
                palette[pid] = (r, g, b)
        elif state == "paint":
            tokens = line.split()
            row = []
            for token in tokens:
                parts = token.rsplit('|', 2)
                if len(parts) >= 3:
                    glyph = parts[0]
                    fg = int(parts[1]) if parts[1] else 0
                    bg = int(parts[2]) if parts[2] else 0
                    if not glyph:
                        glyph = " "
                    row.append((glyph, fg, bg))
            grid.append(row)

    return palette, cols, rows, grid


def main(stdscr):
    locale.setlocale(locale.LC_ALL, "")

    if len(sys.argv) > 1:
        fp = open(sys.argv[1], "r")
    else:
        fp = sys.stdin

    palette, cols, rows, grid = parse_dump(fp)
    if fp is not sys.stdin:
        fp.close()

    curses.start_color()
    curses.use_default_colors()

    stdscr.clear()
    draw_grid(stdscr, palette, grid)
    stdscr.getch()


if __name__ == "__main__":
    try:
        curses.wrapper(main)
    except Exception as e:
        log_error("paint.py main exception", e)
