"""Rasterize the companion artwork and font, then build a bootable ESP32-C3 image."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 128, 160
GLYPH_WIDTH, GLYPH_HEIGHT = 12, 14
PORTRAIT_SHIFT_DOWN = 10
DEFAULT_FONT = "/System/Library/Fonts/Hiragino Sans GB.ttc"


def rgb565(color):
    r, g, b = color
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def common_characters():
    chars = set(chr(code) for code in range(32, 127)) | set("，。！？、…「」《》：；（）—￥·")
    # GB2312 level 1 provides common Chinese while keeping the larger font in RAM.
    for lead in range(0xB0, 0xD8):
        for trail in range(0xA1, 0xFF):
            try:
                chars.add(bytes((lead, trail)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return sorted(chars, key=ord)


def glyph_pixels(font, character):
    width = GLYPH_WIDTH if ord(character) > 127 else max(3, round(font.getlength(character)))
    width = min(width, GLYPH_WIDTH)
    mask = Image.new("L", (GLYPH_WIDTH, GLYPH_HEIGHT))
    ImageDraw.Draw(mask).text((width / 2, GLYPH_HEIGHT / 2), character, font=font, anchor="mm", fill=255)
    bits = bytearray(GLYPH_WIDTH * GLYPH_HEIGHT // 4)
    for index, value in enumerate(mask.get_flattened_data()):
        coverage = (value * 3 + 127) // 255
        bits[index // 4] |= coverage << (2 * (index % 4))
    return width, bits


def generate(output, font_path, font_index):
    with Image.open(ROOT / "assets" / "idle.jpg") as original:
        art = ImageOps.contain(ImageOps.exif_transpose(original).convert("RGB"),
                               (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(art, ((WIDTH - art.width) // 2,
                       (HEIGHT - art.height) // 2 + PORTRAIT_SHIFT_DOWN))
    rotated = canvas.transpose(Image.Transpose.ROTATE_180)
    pixels = b"".join(rgb565(pixel).to_bytes(2, "big") for pixel in rotated.get_flattened_data())
    font = ImageFont.truetype(font_path, 11, index=font_index)
    with (output / "pet_assets.inc").open("w", encoding="utf-8") as stream:
        stream.write("_Alignas(4) static const uint8_t idle_art[] = {\n")
        for offset in range(0, len(pixels), 16):
            stream.write("    " + ", ".join(f"0x{byte:02x}" for byte in pixels[offset:offset+16]) + ",\n")
        stream.write("};\nstatic const Glyph glyphs[] = {\n")
        for character in common_characters():
            width, bits = glyph_pixels(font, character)
            bitmap = ", ".join(f"0x{byte:02x}" for byte in bits)
            stream.write(f"    {{0x{ord(character):04x}, {width}, {{{bitmap}}}}},\n")
        stream.write("};\n")
    canvas.save(output / "idle-preview.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--zig", default="zig")
    parser.add_argument("--font", default=DEFAULT_FONT)
    parser.add_argument("--font-index", type=int)
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    font_index = args.font_index
    if font_index is None:
        font_index = 2 if args.font == DEFAULT_FONT else 0
    generate(output, args.font, font_index)
    env = os.environ.copy()
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(output / "zig-cache"))
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(output / "zig-local-cache"))
    fw = ROOT / "firmware"
    elf = output / "companion.elf"
    subprocess.run([
        args.zig, "cc", "-target", "riscv32-freestanding-none",
        "-mcpu=generic_rv32+m+c+zicsr", "-mabi=ilp32", "-msmall-data-limit=0", "-mno-relax",
        "-ffreestanding", "-fno-stack-protector", "-nostdlib", "-O2", "-Wall", "-Wextra", "-Werror",
        f"-I{output}", f"-Wl,-T,{fw / 'ram.ld'}",
        str(fw / "start.S"), str(fw / "board.c"), str(fw / "pet.c"), "-o", str(elf),
    ], check=True, env=env)
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32c3", "elf2image",
                    "--flash-mode", "dio", "--flash-size", "4MB", "--flash-freq", "40m",
                    "-o", str(output / "companion-v0.1.bin"), str(elf)], check=True)


if __name__ == "__main__":
    main()
