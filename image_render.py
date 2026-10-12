import io
import shutil
import sys
from PIL import Image
import fetch

BLOCKS = [
    ' ', '▗', '▖', '▄', '▝', '▐', '▞', '▟',
    '▘', '▚', '▌', '▙', '▀', '▜', '▛', '█',
]

def load_image(src):
    if isinstance(src, bytes):
        return Image.open(io.BytesIO(src)).convert("RGB")
    if isinstance(src, str):
        if src.startswith(("http://", "https://")):
            return Image.open(io.BytesIO(fetch.fetch_image(src))).convert("RGB")
        return Image.open(src).convert("RGB")
    return Image.open(src).convert("RGB")

def avg(pixels):
    n = len(pixels)
    return (
        sum(p[0] for p in pixels) // n,
        sum(p[1] for p in pixels) // n,
        sum(p[2] for p in pixels) // n,
    )

def render(src, x=0, y=0, scale=1, cols=None, rows=None):
    img = load_image(src)
    w, h = img.size

    x = max(0, min(x, w - 1))
    y = max(0, min(y, h - 1))
    cw = max(1, int(w / max(scale, 1)))
    ch_ = max(1, int(h / max(scale, 1)))
    img = img.crop((x, y, min(x + cw, w), min(y + ch_, h)))
    w, h = img.size

    term = shutil.get_terminal_size((80, 24))
    max_cols = cols or term.columns
    max_rows = rows or max(term.lines - 1, 1)

    out_cols = max_cols
    out_rows = max(1, round(out_cols * h / w / 2))
    if out_rows > max_rows:
        out_rows = max_rows
        out_cols = max(1, round(out_rows * 2 * w / h))

    img = img.resize((out_cols * 2, out_rows * 2), Image.LANCZOS)
    px = img.load()

    lines = []
    for r in range(out_rows):
        parts = []
        for c in range(out_cols):
            quad = [
                px[c * 2, r * 2],
                px[c * 2 + 1, r * 2],
                px[c * 2, r * 2 + 1],
                px[c * 2 + 1, r * 2 + 1],
            ]
            lums = [int(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) for p in quad]
            mean = sum(lums) / 4
            fg = [p for p, l in zip(quad, lums) if l > mean]
            bg = [p for p, l in zip(quad, lums) if l <= mean]

            if not fg:
                br, bg_, bb = avg(quad)
                parts.append(f"\033[48;2;{br};{bg_};{bb}m ")
                continue

            idx = 0
            for bit, l in zip((8, 4, 2, 1), lums):
                if l > mean:
                    idx |= bit

            fr, fg_, fb = avg(fg)
            br, bg_, bb = avg(bg)
            parts.append(
                f"\033[38;2;{fr};{fg_};{fb}m\033[48;2;{br};{bg_};{bb}m{BLOCKS[idx]}"
            )
        lines.append("".join(parts) + "\033[0m")

    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


if __name__ == "__main__":
    args = sys.argv[1:]
    src = args[0] if args else None
    x = int(args[1]) if len(args) > 1 else 0
    y = int(args[2]) if len(args) > 2 else 0
    scale = float(args[3]) if len(args) > 3 else 1
    if not src:
        print("Usage: python image_render.py <image_path_or_url> [x] [y] [zoom]")
        sys.exit(1)
    render(src, x, y, scale)
