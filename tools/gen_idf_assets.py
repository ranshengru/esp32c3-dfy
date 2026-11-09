"""Build a Noto Sans CJK SC glyph subset and v0.1 portrait bytes for the IDF app.

The glyph table is ASCII, every non-ASCII character that appears in string
literals under firmware/idf/main, GB2312 punctuation (rows 1 and 3), and the
3755 level-1 GB2312 hanzi so names, places and messages typed on the settings
page render. Anything else is drawn as "?". Output matches tools/build.py:
idle.jpg is contained in 128x160, shifted down 10px, then stored rotated 180
degrees as big-endian RGB565.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 128, 160
GLYPH_HEIGHT = 16
GLYPH_SLOTS = 16
PORTRAIT_SHIFT_DOWN = 10
INK_THRESHOLD = 128
FONT_SIZE = 16
BIG_SIZE = 31          # clock digits on the home screen
BIG_HEIGHT = 25
BIG_SLOTS = 24
BIG_CHARS = "0123456789:"
# Bubble text uses a smaller face so a sentence fits in two lines.
SMALL_SIZE = 12
SMALL_HEIGHT = 14
# Must match display.c: inner text width of the widest bubble and line limit.
BUBBLE_LINE_WIDTH = 104
BUBBLE_MAX_LINES = 2
# Kaomoji pieces; Noto CJK lacks a few, those come from DejaVu Sans.
KAOMOJI = "´・ω｀`｡•ᴗ✧＞＜ε∀◕‿ﾉ∇≧≦▽ノﾟ∩♡゜°^_-~"
FALLBACK_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")

STRING_RE = re.compile(r'"((?:\\.|[^"\\\n])*)"')
ESCAPE_RE = re.compile(r'\\(?:x([0-9a-fA-F]{2})|([nrt\\"]))')


def rgb565(color):
    r, g, b = color
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def unescape(literal):
    def replace(match):
        hex_byte, simple = match.group(1), match.group(2)
        if hex_byte is not None:
            return chr(int(hex_byte, 16))
        return {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"'}.get(simple, simple)

    return ESCAPE_RE.sub(replace, literal)


def characters_from_sources():
    found = {chr(code) for code in range(32, 127)}
    main = ROOT / "firmware" / "idf" / "main"
    for path in sorted(main.glob("*.c")) + sorted(main.glob("*.h")):
        text = path.read_text(encoding="utf-8")
        for literal in STRING_RE.findall(text):
            for char in unescape(literal):
                if char.isprintable() and ord(char) <= 0xFFFF:
                    found.add(char)
    found.update(gb2312_common())
    found.update(KAOMOJI)
    extra = ROOT / "assets" / "extra_chars.txt"
    if extra.exists():
        for line in extra.read_text(encoding="utf-8").splitlines():
            if not line.startswith("#"):
                found.update(c for c in line.strip() if ord(c) <= 0xFFFF)
    return sorted(found, key=ord)


def gb2312_common():
    chars = set()
    rows = [0xA1, 0xA3] + list(range(0xB0, 0xD8))
    for hi in rows:
        for lo in range(0xA1, 0xFF):
            try:
                char = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if char.isprintable() and ord(char) <= 0xFFFF:
                chars.add(char)
    return chars


def load_font(path, size, index):
    if path.suffix.lower() in {".ttc", ".otc"}:
        return ImageFont.truetype(path, size, index=index)
    return ImageFont.truetype(path, size)


def default_fonts():
    path = ROOT / "assets" / "fonts" / "NotoSansCJKsc-Regular.otf"
    if not path.exists():
        raise SystemExit(f"missing bundled font: {path}; pass --font to use another font")
    return path, 0


_REFERENCE_TOP = {}


def reference_top(font, cell=GLYPH_HEIGHT):
    """Top row of the 16px cell, chosen so a full-height hanzi is centred.

    Every glyph shares this baseline, so punctuation and symbols such as
    '.', ',' and the degree sign keep their natural height instead of being
    centred in the cell.
    """
    key = (id(font), cell)
    if key not in _REFERENCE_TOP:
        canvas = Image.new("L", (48, 40), 0)
        ImageDraw.Draw(canvas).text((0, 4), "国", font=font, fill=255)
        box = canvas.getbbox()
        height = box[3] - box[1]
        _REFERENCE_TOP[key] = box[1] - max(0, (cell - height) // 2)
    return _REFERENCE_TOP[key]


class FaceSet:
    """Primary CJK face plus a fallback for kaomoji symbols it lacks."""

    def __init__(self, path, size, index):
        from fontTools.ttLib import TTFont

        self.primary = load_font(path, size, index)
        self.cmap = TTFont(str(path), fontNumber=index).getBestCmap() if path.suffix.lower() in {".ttc", ".otc"} else TTFont(str(path)).getBestCmap()
        self.fallback = ImageFont.truetype(str(FALLBACK_FONT), size) if FALLBACK_FONT.exists() else None

    def pick(self, character):
        if ord(character) in self.cmap or not self.fallback:
            return self.primary
        return self.fallback


def raster(faces, character, cell=GLYPH_HEIGHT, threshold=INK_THRESHOLD):
    if character == " ":
        return 0, 4 if cell >= 16 else 3, bytes(cell * 2)
    font = faces.pick(character) if isinstance(faces, FaceSet) else faces
    base = faces.primary if isinstance(faces, FaceSet) else faces
    canvas = Image.new("L", (48, 40), 0)
    ImageDraw.Draw(canvas).text((0, 4), character, font=font, fill=255)
    box = canvas.getbbox()
    if not box:
        return 0, 4 if cell >= 16 else 3, bytes(cell * 2)
    top = reference_top(base, cell)
    crop = canvas.crop((box[0], top, box[2], top + cell))
    if not crop.getbbox():
        # Glyph sits entirely outside the cell; fall back to centring it.
        crop = canvas.crop(box)
        if crop.height > cell:
            crop = crop.resize((crop.width, cell), Image.Resampling.BOX)
    if crop.width > GLYPH_SLOTS:
        crop = crop.resize((GLYPH_SLOTS, crop.height), Image.Resampling.BOX)
    placed = Image.new("L", (GLYPH_SLOTS, cell), 0)
    placed.paste(crop, (0, max(0, (cell - crop.height) // 2)))
    bits = bytearray()
    width = 0
    for y in range(cell):
        row = 0
        for x in range(GLYPH_SLOTS):
            if placed.getpixel((x, y)) >= threshold:
                row |= 1 << (15 - x)
                width = max(width, x + 1)
        bits.append((row >> 8) & 0xFF)
        bits.append(row & 0xFF)
    advance = min(GLYPH_SLOTS, width + 1) if width else (4 if cell >= 16 else 3)
    return width, advance, bytes(bits)


def raster_big(font, character):
    """Clock digits: 24x30 1bpp cells, emboldened by one pixel, shared baseline."""
    canvas = Image.new("L", (64, 64), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((4, 4), "0", font=font, fill=255)
    ref = canvas.getbbox()
    canvas = Image.new("L", (64, 64), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((4, 4), character, font=font, fill=255)
    draw.text((5, 4), character, font=font, fill=255)
    box = canvas.getbbox()
    top = ref[1] - max(0, (BIG_HEIGHT - (ref[3] - ref[1])) // 2)
    crop = canvas.crop((box[0], top, box[2], top + BIG_HEIGHT))
    if crop.width > BIG_SLOTS:
        crop = crop.resize((BIG_SLOTS, BIG_HEIGHT), Image.Resampling.BOX)
    bits = bytearray()
    width = 0
    for y in range(BIG_HEIGHT):
        row = 0
        for x in range(min(crop.width, BIG_SLOTS)):
            if crop.getpixel((x, y)) >= INK_THRESHOLD:
                row |= 1 << (BIG_SLOTS - 1 - x)
                width = max(width, x + 1)
        bits += row.to_bytes(3, "big")
    advance = width + 2
    return width, advance, bytes(bits)


def portrait_bytes():
    with Image.open(ROOT / "assets" / "idle.jpg") as original:
        art = ImageOps.contain(
            ImageOps.exif_transpose(original).convert("RGB"),
            (WIDTH, HEIGHT),
            Image.Resampling.LANCZOS,
        )
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(
        art,
        ((WIDTH - art.width) // 2, (HEIGHT - art.height) // 2 + PORTRAIT_SHIFT_DOWN),
    )
    # idle.jpg has two small thought-bubble circles drawn above the head;
    # paint them out so the oval bubble has no trailing dots.
    ImageDraw.Draw(canvas).rectangle((27, 51, 39, 61), fill="white")
    ImageDraw.Draw(canvas).rectangle((32, 62, 38, 67), fill="white")
    rotated = canvas.transpose(Image.Transpose.ROTATE_180)
    pixels = b"".join(rgb565(pixel).to_bytes(2, "big") for pixel in rotated.get_flattened_data())
    return canvas, pixels


BREAK_AFTER = set("，。！？、；：…）)~")
CAKE = 0x1F382
CAKE_ADVANCE = 14


def text_width(text, advances):
    return sum(CAKE_ADVANCE if ord(c) == CAKE else advances.get(ord(c), advances.get(ord("?"), 8)) for c in text)


def layout_bubble(text, advances):
    """Mirror of display.c bubble_layout(): None when it needs > 2 lines."""
    lines = []
    for segment in text.split("\n"):
        segment = segment.strip(" ")
        if not segment:
            continue
        if text_width(segment, advances) <= BUBBLE_LINE_WIDTH:
            lines.append(segment)
            continue
        # Cut priority: right after punctuation, then at a space, then any
        # position that is not inside a parenthesised kaomoji.
        depth, inside = 0, []
        for c in segment:
            inside.append(depth > 0)
            depth += 1 if c in "(（" else -1 if c in ")）" and depth else 0
        best, best_worst = {}, 0
        for i in range(len(segment) - 1, 0, -1):
            left, right = segment[:i], segment[i:].lstrip(" ")
            if inside[i]:
                continue
            if text_width(left, advances) > BUBBLE_LINE_WIDTH or text_width(right, advances) > BUBBLE_LINE_WIDTH:
                continue
            rank = 0 if segment[i - 1] in BREAK_AFTER else 1 if segment[i] == " " else 2
            if rank == 2:
                # No natural break: keep the two lines as even as possible.
                worst = max(text_width(left, advances), text_width(right, advances))
                if 2 not in best or worst < best_worst:
                    best[2], best_worst = i, worst
            else:
                best.setdefault(rank, i)
        best = best[min(best)] if best else None
        if best is None:
            return None
        lines += [segment[:best], segment[best:].lstrip(" ")]
    return lines if 0 < len(lines) <= BUBBLE_MAX_LINES else None


WISH_RE = re.compile(r'BIRTHDAY_WISHES\[\] = \{(.*?)\};', re.S)
# Worst-case name for the birthday wish check: three full-width hanzi.
WISH_TEST_NAME = "國國國"

QUOTE_RE = re.compile(r'\{\s*Q_[A-Z]+\s*,\s*"((?:\\.|[^"\\])*)"\s*\}')


def check_quotes(small):
    """Every built-in bubble line must render with known glyphs in <= 2 lines."""
    advances = {code: adv for code, _w, adv, _b in small}
    text = (ROOT / "firmware" / "idf" / "main" / "quotes.h").read_text(encoding="utf-8")
    quotes = [unescape(q) for q in QUOTE_RE.findall(text)]
    bad = []
    for quote in quotes:
        missing = [c for c in quote if ord(c) not in advances and c != "\n" and ord(c) != CAKE]
        if missing or layout_bubble(quote, advances) is None:
            bad.append((quote, text_width(quote, advances), "".join(missing)))
    block = WISH_RE.search(text)
    wishes = [unescape(w) for w in STRING_RE.findall(block.group(1))] if block else []
    for wish in wishes:
        line = wish.replace("{n}", WISH_TEST_NAME)
        missing = [c for c in line if ord(c) not in advances and c != "\n" and ord(c) != CAKE]
        if "{n}" not in wish or missing or layout_bubble(line, advances) is None:
            bad.append((line, text_width(line, advances), "".join(missing)))
    if len(set(quotes)) != len(quotes):
        dupes = sorted({q for q in quotes if quotes.count(q) > 1})
        raise SystemExit(f"duplicate quotes: {dupes}")
    if bad:
        for quote, width, missing in bad:
            print(f"OVERFLOW {width}px {quote!r} missing={missing!r}")
        raise SystemExit(f"{len(bad)} quote(s) do not fit {BUBBLE_MAX_LINES} lines of {BUBBLE_LINE_WIDTH}px")
    return quotes + wishes


def write_glyphs(path, glyphs, big=(), small=()):
    lines = [
        "#ifndef COMPANION_GLYPHS_H",
        "#define COMPANION_GLYPHS_H",
        "",
        "#include <stdint.h>",
        "",
        f"enum {{ GLYPH_HEIGHT = {GLYPH_HEIGHT} }};",
        "",
        "typedef struct {",
        "    uint16_t codepoint;",
        "    uint8_t width;",
        "    uint8_t advance;",
        f"    uint8_t bits[GLYPH_HEIGHT * 2];",
        "} Glyph;",
        "",
        "static const Glyph glyphs[] = {",
    ]
    for codepoint, width, advance, bits in glyphs:
        blob = ", ".join(f"0x{byte:02x}" for byte in bits)
        lines.append(f"    {{0x{codepoint:04x}, {width}, {advance}, {{{blob}}}}},")
    lines.append("};")
    lines.append(f"enum {{ GLYPH_COUNT = {len(glyphs)} }};")
    lines.append("")
    lines.append(f"enum {{ BIG_HEIGHT = {BIG_HEIGHT}, BIG_SLOTS = {BIG_SLOTS} }};")
    lines.append("typedef struct { char ch; uint8_t width; uint8_t advance; uint8_t bits[BIG_HEIGHT * 3]; } BigGlyph;")
    lines.append("static const BigGlyph big_glyphs[] = {")
    for ch, width, advance, bits in big:
        blob = ", ".join(f"0x{byte:02x}" for byte in bits)
        lines.append(f"    {{'{ch}', {width}, {advance}, {{{blob}}}}},")
    lines.append("};")
    lines.append(f"enum {{ BIG_COUNT = {len(big)} }};")
    lines.append("")
    lines.append(f"enum {{ SMALL_HEIGHT = {SMALL_HEIGHT} }};")
    lines.append("typedef struct { uint16_t codepoint; uint8_t width; uint8_t advance; uint8_t bits[SMALL_HEIGHT * 2]; } SmallGlyph;")
    lines.append("static const SmallGlyph small_glyphs[] = {")
    for codepoint, width, advance, bits in small:
        blob = ", ".join(f"0x{byte:02x}" for byte in bits)
        lines.append(f"    {{0x{codepoint:04x}, {width}, {advance}, {{{blob}}}}},")
    lines.append("};")
    lines.append(f"enum {{ SMALL_COUNT = {len(small)} }};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def preview(canvas, glyphs, destination):
    by_code = {item[0]: item for item in glyphs}

    def blit(image, x, y, codepoint):
        item = by_code.get(codepoint) or by_code.get(ord("?"))
        if not item:
            return 8
        _code, width, advance, bits = item
        for row in range(GLYPH_HEIGHT):
            value = (bits[row * 2] << 8) | bits[row * 2 + 1]
            for col in range(min(width, 16)):
                if value & (1 << (15 - col)):
                    px, py = x + col, y + row
                    if 0 <= px < WIDTH and 0 <= py < HEIGHT:
                        image.putpixel((px, py), (16, 28, 56))
        return advance

    frames = []
    samples = [
        ["配网热点", "Pet-Setup-8805", "密码 12345678", "192.168.4.1"],
        ["21:09 周一", "18°C 多云"],
        ["21:09 周一", "7°C 晴", "该喝水了"],
    ]
    for lines in samples:
        frame = canvas.copy()
        y = 2
        for text in lines:
            if y + GLYPH_HEIGHT > 66:
                break
            for row in range(y - 1, y - 1 + GLYPH_HEIGHT + 2):
                for col in range(WIDTH):
                    frame.putpixel((col, row), (255, 255, 255))
            x = 4
            for char in text:
                if x >= WIDTH - 4:
                    break
                x += blit(frame, x, y, ord(char))
            y += 16
        frames.append(frame.resize((WIDTH * 4, HEIGHT * 4), Image.Resampling.NEAREST))
    sheet = Image.new("RGB", (WIDTH * 4 * len(frames), HEIGHT * 4), "white")
    for index, frame in enumerate(frames):
        sheet.paste(frame, (index * WIDTH * 4, 0))
    sheet.save(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font")
    parser.add_argument("--font-index", type=int)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    if args.font:
        font_path = Path(args.font)
        font_index = 0 if args.font_index is None else args.font_index
    else:
        font_path, font_index = default_fonts()
        if args.font_index is not None:
            font_index = args.font_index
    font = FaceSet(font_path, FONT_SIZE, font_index)
    small_font = FaceSet(font_path, SMALL_SIZE, font_index)
    glyphs = []
    small = []
    for char in characters_from_sources():
        width, advance, bits = raster(font, char)
        glyphs.append((ord(char), width, advance, bits))
        width, advance, bits = raster(small_font, char, SMALL_HEIGHT, 100)
        small.append((ord(char), width, advance, bits))
    quotes = check_quotes(small)
    main_dir = ROOT / "firmware" / "idf" / "main"
    big_font = load_font(font_path, BIG_SIZE, font_index)
    big = [(ch, *raster_big(big_font, ch)) for ch in BIG_CHARS]
    write_glyphs(main_dir / "glyphs.h", glyphs, big, small)
    canvas, pixels = portrait_bytes()
    (main_dir / "idle.rgb565").write_bytes(pixels)
    preview_path = args.preview or (ROOT / "firmware" / "idf" / "status-preview.png")
    preview(canvas, glyphs, preview_path)
    print(f"quotes {len(quotes)} ok (<= {BUBBLE_MAX_LINES} lines of {BUBBLE_LINE_WIDTH}px)")
    print(f"glyphs {len(glyphs)} small {len(small)} idle {len(pixels)} preview {preview_path}")


if __name__ == "__main__":
    main()
