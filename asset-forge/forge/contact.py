"""Contact sheets: many trees on one page, which is how choosing actually works.

Every cell carries its seed and its measured height, and anything the health
check flagged gets a marked corner and a reason. A judgement is only useful if
it is attached to something reproducible, so the sheet header carries the spec
hash that produced it -- the same spec hash regenerates every tree on the page.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
PANEL = (28, 31, 36)
INK = (226, 230, 236)
DIM = (140, 148, 160)
WARN = (226, 142, 60)


def _font(size: int):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def sheet(
    cells: list[tuple[Image.Image, str, list[str]]],
    *,
    title: str,
    subtitle: str = "",
    columns: int = 6,
    cell: tuple[int, int] | None = None,
    max_cell: tuple[int, int] = (420, 620),
) -> Image.Image:
    """`cells` is (thumbnail, label, problems).

    With no explicit cell size the page sizes itself to the tallest tree on it.
    Fixed cells plus a uniform scale would leave a page of mostly empty sky
    whenever one species dwarfs the rest, which is exactly the case a survey
    puts on screen.
    """
    label_h = 30
    pad = 8
    head = 56
    columns = max(1, min(columns, len(cells) or 1))
    rows = (len(cells) + columns - 1) // columns

    # One shrink factor for the whole page. Resizing each thumbnail to fill its
    # own cell would erase the size difference between species, which is the
    # thing a sheet exists to show.
    max_w = max(t.width for t, _, _ in cells)
    max_h = max(t.height for t, _, _ in cells)
    if cell is None:
        factor = min(1.0, (max_cell[0] - 12) / max_w, (max_cell[1] - 12) / max_h)
        cw = int(max_w * factor) + 12
    else:
        cw, ch_fixed = cell
        factor = min(1.0, (cw - 8) / max_w, (ch_fixed - 8) / max_h)

    def fit(t: Image.Image) -> Image.Image:
        if factor >= 1.0:
            return t
        return t.resize(
            (max(1, int(t.width * factor)), max(1, int(t.height * factor))), Image.NEAREST
        )

    fitted = [fit(t) for t, _, _ in cells]

    # Rows are sized to their own tallest tree rather than the page's. A survey
    # that puts a 4.5 m sapling and a 28 m emergent on one page would otherwise
    # be mostly empty sky.
    if cell is None:
        row_h = [
            max(fitted[i].height for i in range(r * columns, min((r + 1) * columns, len(fitted))))
            for r in range(rows)
        ]
    else:
        row_h = [ch_fixed] * rows
    row_y = []
    y = head
    for h in row_h:
        row_y.append(y)
        y += h + label_h + pad

    img = Image.new("RGB", (columns * (cw + pad) + pad, y + pad), BG)
    draw = ImageDraw.Draw(img)
    draw.text((pad + 2, 12), title, fill=INK, font=_font(20))
    if subtitle:
        draw.text((pad + 2, 34), subtitle, fill=DIM, font=_font(13))

    small = _font(12)
    for i, (_, label, problems) in enumerate(cells):
        r, c = divmod(i, columns)
        cx = pad + c * (cw + pad)
        cy = row_y[r]
        ch = row_h[r]
        draw.rectangle([cx, cy, cx + cw, cy + ch + label_h], fill=PANEL)

        thumb = fitted[i]
        ox = cx + (cw - thumb.width) // 2
        oy = cy + (ch - thumb.height)  # sit trees on the cell floor
        img.paste(thumb, (ox, oy), thumb if thumb.mode == "RGBA" else None)

        draw.text((cx + 6, cy + ch + 4), label, fill=INK, font=small)
        if problems:
            draw.rectangle([cx, cy, cx + 6, cy + 6], fill=WARN)
            draw.text((cx + 6, cy + ch + 17), problems[0][:44], fill=WARN, font=_font(11))
    return img


def save(img: Image.Image, path: str | Path) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    return path
