"""Build the small status font and the v0.1 portrait bytes for the IDF app.

The freestanding image keeps a full GB2312 font in DRAM. This stage only needs
the status strings, so the glyph table is the ASCII set plus every non-ASCII
character that appears in firmware/idf/main. Output matches tools/build.py:
idle.jpg is contained in 128x160, shifted down 10px, then stored rotated 180
degrees as big-endian RGB565.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 128, 160
GLYPH_HEIGHT = 14
GLYPH_SLOTS = 16
PORTRAIT_SHIFT_DOWN = 10
INK_THRESHOLD = 128

STRING_RE = re.compile(r'"((?:\\.|[^"\\])*)"')
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
    return sorted(found, key=ord)


def load_font(path, size, index):
    if path.suffix.lower() in {".ttc", ".otc"}:
        return ImageFont.truetype(path, size, index=index)
    return ImageFont.truetype(path, size)


def default_fonts():
    candidates = [
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
        Path("/System/Library/Fonts/Hiragino Sans GB.ttc"),
        Path("/System/Library/Fonts/STHeiti Medium.ttc"),
    ]
    for path in candidates:
        if path.exists():
            index = 2 if "Hiragino" in path.name else 0
            return path, index
    raise SystemExit("no CJK font found; pass --font")


def raster(font, character):
    if character == " ":
        return 0, 4, bytes(GLYPH_HEIGHT * 2)
    canvas = Image.new("L", (48, 32), 0)
    ImageDraw.Draw(canvas).text((0, 0), character, font=font, fill=255)
    box = canvas.getbbox()
    if not box:
        return 0, 4, bytes(GLYPH_HEIGHT * 2)
    crop = canvas.crop(box)
    if crop.width > GLYPH_SLOTS or crop.height > GLYPH_HEIGHT:
        crop.thumbnail((GLYPH_SLOTS, GLYPH_HEIGHT), Image.Resampling.BOX)
    placed = Image.new("L", (GLYPH_SLOTS, GLYPH_HEIGHT), 0)
    placed.paste(crop, (0, max(0, (GLYPH_HEIGHT - crop.height) // 2)))
    bits = bytearray()
    width = 0
    for y in range(GLYPH_HEIGHT):
        row = 0
        for x in range(GLYPH_SLOTS):
            if placed.getpixel((x, y)) >= INK_THRESHOLD:
                row |= 1 << (15 - x)
                width = max(width, x + 1)
        bits.append((row >> 8) & 0xFF)
        bits.append(row & 0xFF)
    advance = 4 if width == 0 else min(GLYPH_SLOTS, width + 1)
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
    rotated = canvas.transpose(Image.Transpose.ROTATE_180)
    pixels = b"".join(rgb565(pixel).to_bytes(2, "big") for pixel in rotated.get_flattened_data())
    return canvas, pixels


def write_glyphs(path, glyphs):
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
        ["小粤", "未设置网络", "SSID 网络名", "KEY 密钥"],
        ["小粤", "已连接 21:09", "余额 12.50 CNY", "扫一下刷新"],
        ["小粤", "连接中", "demo-ssid", "查询失败"],
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
    font = load_font(font_path, 13, font_index)
    glyphs = []
    for char in characters_from_sources():
        width, advance, bits = raster(font, char)
        glyphs.append((ord(char), width, advance, bits))
    main_dir = ROOT / "firmware" / "idf" / "main"
    write_glyphs(main_dir / "glyphs.h", glyphs)
    canvas, pixels = portrait_bytes()
    (main_dir / "idle.rgb565").write_bytes(pixels)
    preview_path = args.preview or (ROOT / "firmware" / "idf" / "status-preview.png")
    preview(canvas, glyphs, preview_path)
    print(f"glyphs {len(glyphs)} idle {len(pixels)} preview {preview_path}")


if __name__ == "__main__":
    main()
