#!/usr/bin/env python3
"""Pack the classic portrait and eight expression loops into assets.bin.

Each image is the full 128x160 screen, stored like idle.rgb565 (shifted down
10px, rotated 180 degrees, big-endian RGB565). It is split into 16-line
strips and LZ4-HC compressed independently so the firmware can mmap the
bytes in flash and decode one strip into a 4 KB buffer.

Classic art is taken from firmware/idf/main/idle.rgb565 (the same bytes as
v0.5 / v1.0 classic mode) and also written as firmware/idf/main/idle.lz4s
for the no-pack fallback. Expression frames are read from
assets/expressions/<name>_<frame>.png; the original videos are not required.
"""

import argparse
import ctypes
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "expressions"
BIN_PATH = ROOT / "firmware" / "idf" / "assets.bin"
FALLBACK_PATH = ROOT / "firmware" / "idf" / "main" / "idle.lz4s"
IDLE_RAW = ROOT / "firmware" / "idf" / "main" / "idle.rgb565"
PREVIEW = ROOT / "docs" / "v1.0.1-expressions-preview.png"
FRAMES_PREVIEW = ROOT / "docs" / "v1.0.1-frames-preview.png"

WIDTH, HEIGHT = 128, 160
SHIFT = 10
RAW = WIDTH * HEIGHT * 2
STRIP_LINES = 16
PACK_FORMAT = 3
PACK_VERSION = 101
FLAG_LZ4_STRIPS = 1
CODEC_LZ4_STRIPS = 1
EXPR_CLASSIC = 0xFF
HEADER_SIZE = 32

EXPRS = [
    ("neutral", "平常"),
    ("happy", "开心"),
    ("sleepy", "困"),
    ("sleeping", "打盹"),
    ("tsundere", "傲娇"),
    ("surprised", "惊讶"),
    ("worried", "担心"),
    ("excited", "兴奋"),
]


def rgb565(color):
    r, g, b = color
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def load_lz4():
    lib = ctypes.CDLL("liblz4.so.1")
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int
    lib.LZ4_compress_HC.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int
    ]
    lib.LZ4_compress_HC.restype = ctypes.c_int
    lib.LZ4_decompress_safe.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int
    ]
    lib.LZ4_decompress_safe.restype = ctypes.c_int
    return lib


LZ4 = load_lz4()


def lz4_hc(data, level=12):
    bound = LZ4.LZ4_compressBound(len(data))
    buf = ctypes.create_string_buffer(bound)
    n = LZ4.LZ4_compress_HC(data, buf, len(data), bound, level)
    if n <= 0:
        raise SystemExit("LZ4_compress_HC failed")
    return buf.raw[:n]


def lz4_decomp(comp, raw_len):
    out = ctypes.create_string_buffer(raw_len)
    n = LZ4.LZ4_decompress_safe(comp, out, len(comp), raw_len)
    if n != raw_len:
        raise SystemExit(f"LZ4_decompress_safe returned {n}, expected {raw_len}")
    return out.raw[:n]


def screen_canvas(im):
    art = ImageOps.contain(ImageOps.exif_transpose(im).convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(art, ((WIDTH - art.width) // 2, (HEIGHT - art.height) // 2 + SHIFT))
    return canvas


def canvas_to_raw(canvas):
    rotated = canvas.transpose(Image.Transpose.ROTATE_180)
    raw = b"".join(rgb565(px).to_bytes(2, "big") for px in rotated.get_flattened_data())
    if len(raw) != RAW:
        raise SystemExit(f"frame is {len(raw)} bytes, expected {RAW}")
    return raw


def raw_to_canvas(raw):
    if len(raw) != RAW:
        raise SystemExit(f"raw image is {len(raw)} bytes")
    pixels = []
    for i in range(0, RAW, 2):
        v = (raw[i] << 8) | raw[i + 1]
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 5) & 63) * 255 // 63
        b = (v & 31) * 255 // 31
        pixels.append((r, g, b))
    im = Image.new("RGB", (WIDTH, HEIGHT))
    im.putdata(pixels)
    return im.transpose(Image.Transpose.ROTATE_180)


def encode_image(raw):
    if len(raw) != RAW:
        raise SystemExit(f"encode expected {RAW} bytes, got {len(raw)}")
    count = (HEIGHT + STRIP_LINES - 1) // STRIP_LINES
    sizes = []
    payload = bytearray()
    for i in range(count):
        y0 = i * STRIP_LINES
        h = min(STRIP_LINES, HEIGHT - y0)
        chunk = raw[y0 * WIDTH * 2 : (y0 + h) * WIDTH * 2]
        comp = lz4_hc(chunk)
        if lz4_decomp(comp, len(chunk)) != chunk:
            raise SystemExit("lz4 round-trip failed")
        if len(comp) > 65535:
            raise SystemExit(f"strip compressed to {len(comp)}")
        sizes.append(len(comp))
        payload += comp
    return struct.pack("<BBH", CODEC_LZ4_STRIPS, STRIP_LINES, count) + struct.pack(
        "<" + "H" * count, *sizes
    ) + bytes(payload)


def decode_image(blob):
    if len(blob) < 4:
        raise SystemExit("image blob too short")
    codec, strip_lines, count = struct.unpack_from("<BBH", blob, 0)
    if codec != CODEC_LZ4_STRIPS or strip_lines == 0 or strip_lines > 16 or count == 0:
        raise SystemExit(f"bad image header {codec} {strip_lines} {count}")
    table = 4
    if len(blob) < table + 2 * count:
        raise SystemExit("truncated size table")
    sizes = struct.unpack_from("<" + "H" * count, blob, table)
    at = table + 2 * count
    out = bytearray()
    for i, size in enumerate(sizes):
        chunk = blob[at : at + size]
        if len(chunk) != size:
            raise SystemExit("truncated strip")
        h = min(strip_lines, HEIGHT - i * strip_lines)
        out += lz4_decomp(chunk, h * WIDTH * 2)
        at += size
    if at != len(blob):
        raise SystemExit(f"trailing {len(blob) - at} bytes in image blob")
    if len(out) != RAW:
        raise SystemExit(f"decoded {len(out)} bytes, expected {RAW}")
    return bytes(out)


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def label_font(size=20):
    for path in (
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        str(ROOT / "assets/fonts/NotoSansCJKsc-Regular.otf"),
    ):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def save_sheet(cells, destination, cols, caption=None):
    scale = 3
    pad = 14
    label_h = 28
    cell_w, cell_h = WIDTH * scale, HEIGHT * scale
    rows = (len(cells) + cols - 1) // cols
    extra = 36 if caption else 0
    sheet = Image.new(
        "RGB",
        (cols * cell_w + (cols + 1) * pad, rows * (cell_h + label_h) + (rows + 1) * pad + extra),
        (243, 234, 220),
    )
    draw = ImageDraw.Draw(sheet)
    font = label_font(20)
    cap_font = label_font(18)
    y0 = pad + extra
    if caption:
        draw.text((pad, 10), caption, fill=(28, 25, 22), font=cap_font)
    for i, (label, canvas) in enumerate(cells):
        r, c = divmod(i, cols)
        x = pad + c * (cell_w + pad)
        y = y0 + r * (cell_h + label_h + pad)
        sheet.paste(canvas.resize((cell_w, cell_h), Image.Resampling.NEAREST), (x, y))
        tw = draw.textlength(label, font=font)
        draw.text((x + (cell_w - tw) / 2, y + cell_h + 4), label, fill=(28, 25, 22), font=font)
    destination.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(destination)
    print(f"wrote {destination}")


def write_idle_c(blob):
    path = ROOT / "firmware" / "idf" / "main" / "idle_lz4s.c"
    lines = [
        "/* Auto-generated by tools/gen_expressions.py from idle.lz4s. Do not edit. */",
        "#include <stdint.h>",
        "",
        "const uint8_t idle_lz4s_data[] = {",
    ]
    row = []
    for i, b in enumerate(blob):
        row.append(f"0x{b:02x}")
        if len(row) == 16 or i == len(blob) - 1:
            lines.append("    " + ", ".join(row) + ("," if i != len(blob) - 1 else ""))
            row = []
    lines += ["};", f"const unsigned idle_lz4s_size = {len(blob)};", ""]
    path.write_text("\n".join(lines))
    print(f"wrote {path} {path.stat().st_size} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-dir", type=Path, help="also write PNG previews into this directory")
    args = parser.parse_args()

    if not IDLE_RAW.exists():
        raise SystemExit(f"missing {IDLE_RAW}; run tools/gen_idf_assets.py first")
    classic_raw = IDLE_RAW.read_bytes()
    if len(classic_raw) != RAW:
        raise SystemExit(f"idle.rgb565 is {len(classic_raw)} bytes, expected {RAW}")

    entries = []  # (expr, frame, blob, raw, canvas)
    classic_blob = encode_image(classic_raw)
    if decode_image(classic_blob) != classic_raw:
        raise SystemExit("classic pack pixels differ from idle.rgb565")
    entries.append((EXPR_CLASSIC, 0, classic_blob, classic_raw, raw_to_canvas(classic_raw)))
    print(f"classic: {len(classic_blob)} bytes (raw {RAW}), lossless match to idle.rgb565")

    face_previews = [("经典", entries[0][4])]
    all_frames = []
    for expr_id, (key, label) in enumerate(EXPRS):
        for frame_i in range(4):
            path = OUT_DIR / f"{key}_{frame_i}.png"
            if not path.exists():
                raise SystemExit(f"missing expression frame {path}")
            canvas = screen_canvas(Image.open(path))
            raw = canvas_to_raw(canvas)
            blob = encode_image(raw)
            if decode_image(blob) != raw:
                raise SystemExit(f"{path} round-trip failed")
            entries.append((expr_id, frame_i, blob, raw, canvas))
            all_frames.append((f"{label} {frame_i + 1}", canvas))
            if frame_i == 0:
                face_previews.append((label, canvas))
            print(f"{key} frame {frame_i}: {len(blob)} bytes")

    directory = bytearray()
    payload = bytearray()
    cursor = HEADER_SIZE + len(entries) * 16
    for expr_id, frame_i, blob, _raw, _canvas in entries:
        directory += struct.pack("<BBHIII", expr_id, frame_i, 0, cursor, len(blob), crc32(blob))
        payload += blob
        cursor += len(blob)
    data = bytes(directory) + bytes(payload)
    head = struct.pack(
        "<4sHHIHHBBHII",
        b"DFYA",
        PACK_FORMAT,
        HEADER_SIZE,
        PACK_VERSION,
        WIDTH,
        HEIGHT,
        1,
        FLAG_LZ4_STRIPS,
        len(entries),
        len(data),
        crc32(data),
    )
    if len(head) != 28:
        raise SystemExit(f"header without crc is {len(head)}")
    blob = head + struct.pack("<I", crc32(head)) + data
    BIN_PATH.write_bytes(blob)
    FALLBACK_PATH.write_bytes(classic_blob)
    write_idle_c(classic_blob)
    print(f"wrote {BIN_PATH} {len(blob)} bytes, {len(entries)} images")
    print(f"wrote {FALLBACK_PATH} {len(classic_blob)} bytes")

    # Reload and verify the on-disk pack, including classic identity.
    packed = BIN_PATH.read_bytes()
    if packed != blob:
        raise SystemExit("assets.bin write mismatch")
    magic, fmt, hsize, ver, w, h, pix, flags, count, data_size, data_crc = struct.unpack_from(
        "<4sHHIHHBBHII", packed, 0
    )
    header_crc = struct.unpack_from("<I", packed, 28)[0]
    if magic != b"DFYA" or fmt != PACK_FORMAT or hsize != 32 or ver != PACK_VERSION:
        raise SystemExit("pack header fields wrong")
    if crc32(packed[:28]) != header_crc or crc32(packed[32:32 + data_size]) != data_crc:
        raise SystemExit("pack crc mismatch")
    off = 32
    for expr_id, frame_i, expected, raw, _canvas in entries:
        e_expr, e_frame, _res, e_off, e_size, e_crc = struct.unpack_from("<BBHIII", packed, off)
        off += 16
        got = packed[e_off:e_off + e_size]
        if (e_expr, e_frame, e_size, crc32(got), decode_image(got)) != (
            expr_id, frame_i, len(expected), expected and crc32(expected), raw
        ):
            raise SystemExit(f"pack entry expr={expr_id} frame={frame_i} failed verification")
    print("pack self-check ok: every image lossless, classic identical to idle.rgb565")

    save_sheet(
        face_previews,
        PREVIEW,
        cols=3,
        caption="v1.0.1  经典立绘 + 八个表情（首帧）  循环仍是 4 帧，约 0.9 秒一帧",
    )
    save_sheet(
        all_frames,
        FRAMES_PREVIEW,
        cols=4,
        caption="v1.0.1  八个表情 × 4 帧（约 0.9 秒一帧；两帧循环跳跃太大，仍保留 4 帧）",
    )
    if args.preview_dir:
        out = args.preview_dir
        out.mkdir(parents=True, exist_ok=True)
        classic = entries[0][4]
        classic.resize((WIDTH * 4, HEIGHT * 4), Image.Resampling.NEAREST).save(out / "pet-v101-classic.png")
        # Side-by-side: source idle vs decoded pack (should be identical).
        pair = Image.new("RGB", (WIDTH * 4 * 2 + 24, HEIGHT * 4 + 40), (243, 234, 220))
        draw = ImageDraw.Draw(pair)
        font = label_font(18)
        left = raw_to_canvas(classic_raw).resize((WIDTH * 4, HEIGHT * 4), Image.Resampling.NEAREST)
        right = classic.resize((WIDTH * 4, HEIGHT * 4), Image.Resampling.NEAREST)
        pair.paste(left, (0, 32))
        pair.paste(right, (WIDTH * 4 + 24, 32))
        draw.text((8, 6), "v0.5 / v1.0 经典 idle.rgb565", fill=(28, 25, 22), font=font)
        draw.text((WIDTH * 4 + 32, 6), "v1.0.1 包里解出的经典（应完全一样）", fill=(28, 25, 22), font=font)
        pair.save(out / "pet-v101-classic-compare.png")
        save_sheet(face_previews, out / "pet-v101-screens.png", cols=3, caption="v1.0.1 经典 + 八个表情（首帧）")
        save_sheet(
            all_frames,
            out / "pet-v101-frames.png",
            cols=4,
            caption="v1.0.1 表情循环：每张脸 4 帧，约 0.9 秒一帧",
        )
        # Two-frame vs four-frame, for the record (we keep four).
        two = []
        for expr_id, (key, label) in enumerate(EXPRS):
            two.append((f"{label} 1", screen_canvas(Image.open(OUT_DIR / f"{key}_0.png"))))
            two.append((f"{label} 3", screen_canvas(Image.open(OUT_DIR / f"{key}_2.png"))))
        save_sheet(two, out / "pet-v101-2frame.png", cols=4, caption="仅作对照：每张脸只留第 1、3 帧，跳跃更明显，固件不采用")


if __name__ == "__main__":
    main()
