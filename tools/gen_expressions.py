#!/usr/bin/env python3
"""Pack the eight expression loops into assets.bin.

Each frame is the full 128x160 screen, stored like idle.rgb565 (shifted down
10px, rotated 180 degrees, big-endian RGB565) and left uncompressed. The
firmware maps the pack through the flash cache, so a frame does not occupy
RAM. 困 and 打盹 are separate loops.
"""

import struct
import subprocess
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "expressions"
BIN_PATH = ROOT / "firmware" / "idf" / "assets.bin"
PREVIEW = ROOT / "docs" / "v1.0-expressions-preview.png"
WIDTH, HEIGHT = 128, 160
SHIFT = 10
RAW = WIDTH * HEIGHT * 2
PACK_VERSION = 20261111
VIDEO_DIR = Path("/workspace/artifacts/imagine_videos")
GRAB_DIR = Path("/tmp/expr-frames")

EXPRS = [
    ("neutral", "平常", [
        ("a623c533-d39f-4ead-bb52-7e32291103b7.mp4", 0.6),
        ("a623c533-d39f-4ead-bb52-7e32291103b7.mp4", 2.2),
        ("a623c533-d39f-4ead-bb52-7e32291103b7.mp4", 3.6),
        ("a623c533-d39f-4ead-bb52-7e32291103b7.mp4", 5.0),
    ]),
    ("happy", "开心", [
        ("eca01003-3346-4f02-a288-68ecd509bb7c.mp4", 0.6),
        ("eca01003-3346-4f02-a288-68ecd509bb7c.mp4", 2.0),
        ("eca01003-3346-4f02-a288-68ecd509bb7c.mp4", 3.6),
        ("eca01003-3346-4f02-a288-68ecd509bb7c.mp4", 5.0),
    ]),
    ("sleepy", "困", [
        ("6d54b0f9-2302-47d8-a3fa-371fd43090c3.mp4", 0.6),
        ("6d54b0f9-2302-47d8-a3fa-371fd43090c3.mp4", 2.2),
        ("6d54b0f9-2302-47d8-a3fa-371fd43090c3.mp4", 3.8),
        ("6d54b0f9-2302-47d8-a3fa-371fd43090c3.mp4", 5.0),
    ]),
    ("sleeping", "打盹", [
        ("14b06b9c-750d-459e-8630-d090d3729719.mp4", 0.6),
        ("14b06b9c-750d-459e-8630-d090d3729719.mp4", 2.2),
        ("14b06b9c-750d-459e-8630-d090d3729719.mp4", 3.8),
        ("14b06b9c-750d-459e-8630-d090d3729719.mp4", 5.0),
    ]),
    ("tsundere", "傲娇", [
        ("76fa2233-ec2b-4b7e-bc16-1199af568045.mp4", 0.6),
        ("76fa2233-ec2b-4b7e-bc16-1199af568045.mp4", 2.2),
        ("76fa2233-ec2b-4b7e-bc16-1199af568045.mp4", 3.8),
        ("76fa2233-ec2b-4b7e-bc16-1199af568045.mp4", 5.0),
    ]),
    ("surprised", "惊讶", [
        ("cd7809e1-5258-4d32-bd60-a710be44bf47.mp4", 0.5),
        ("cd7809e1-5258-4d32-bd60-a710be44bf47.mp4", 1.8),
        ("cd7809e1-5258-4d32-bd60-a710be44bf47.mp4", 3.4),
        ("cd7809e1-5258-4d32-bd60-a710be44bf47.mp4", 5.0),
    ]),
    ("worried", "担心", [
        ("dda351ae-9988-417d-9f91-7e155403473f.mp4", 0.6),
        ("dda351ae-9988-417d-9f91-7e155403473f.mp4", 2.2),
        ("dda351ae-9988-417d-9f91-7e155403473f.mp4", 3.8),
        ("dda351ae-9988-417d-9f91-7e155403473f.mp4", 5.0),
    ]),
    ("excited", "兴奋", [
        ("b71c0e08-ed87-4bbe-830c-b5451847074a.mp4", 0.6),
        ("b71c0e08-ed87-4bbe-830c-b5451847074a.mp4", 2.0),
        ("b71c0e08-ed87-4bbe-830c-b5451847074a.mp4", 3.6),
        ("b71c0e08-ed87-4bbe-830c-b5451847074a.mp4", 5.0),
    ]),
]


def rgb565(color):
    r, g, b = color
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def screen_canvas(im):
    art = ImageOps.contain(ImageOps.exif_transpose(im).convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(art, ((WIDTH - art.width) // 2, (HEIGHT - art.height) // 2 + SHIFT))
    return canvas


def grab(video, second, dest):
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(
        ["ffmpeg", "-y", "-ss", str(second), "-i", str(video), "-frames:v", "1", str(dest)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def frame_bytes(canvas):
    rotated = canvas.transpose(Image.Transpose.ROTATE_180)
    raw = b"".join(rgb565(px).to_bytes(2, "big") for px in rotated.getdata())
    if len(raw) != RAW:
        raise SystemExit(f"frame is {len(raw)} bytes, expected {RAW}")
    return raw


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    entries = []
    previews = []
    for expr_id, (key, label, frames) in enumerate(EXPRS):
        for frame_i, (video_name, second) in enumerate(frames):
            png = OUT_DIR / f"{key}_{frame_i}.png"
            if png.exists():
                canvas = Image.open(png).convert("RGB")
                if canvas.size != (WIDTH, HEIGHT):
                    canvas = screen_canvas(canvas)
            else:
                full = GRAB_DIR / f"{key}_{frame_i}.png"
                grab(VIDEO_DIR / video_name, second, full)
                canvas = screen_canvas(Image.open(full))
                canvas.save(png)
            raw = frame_bytes(canvas)
            entries.append((expr_id, frame_i, raw))
            if frame_i == 0:
                previews.append((label, canvas))
            print(f"{key} frame {frame_i}: {len(raw)} bytes in flash")

    directory = bytearray()
    payload = bytearray()
    header_size = 32
    cursor = header_size + len(entries) * 16
    for expr_id, frame_i, comp in entries:
        directory += struct.pack("<BBHIII", expr_id, frame_i, 0, cursor, len(comp), crc32(comp))
        payload += comp
        cursor += len(comp)
    data = bytes(directory) + bytes(payload)
    head = struct.pack(
        "<4sHHIHHBBHII",
        b"DFYA",
        3,
        header_size,
        PACK_VERSION,
        WIDTH,
        HEIGHT,
        1,
        0,
        len(entries),
        len(data),
        crc32(data),
    )
    if len(head) != 28:
        raise SystemExit(f"header without crc is {len(head)}")
    blob = head + struct.pack("<I", crc32(head)) + data
    BIN_PATH.write_bytes(blob)
    print(f"wrote {BIN_PATH} {len(blob)} bytes, {len(entries)} frames")

    scale = 3
    cols = 4
    pad = 14
    label_h = 28
    cell_w, cell_h = WIDTH * scale, HEIGHT * scale
    rows = 2
    sheet = Image.new("RGB", (cols * cell_w + (cols + 1) * pad, rows * (cell_h + label_h) + (rows + 1) * pad), (243, 234, 220))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 20)
    except OSError:
        font = ImageFont.load_default()
    for i, (label, canvas) in enumerate(previews):
        r, c = divmod(i, cols)
        x = pad + c * (cell_w + pad)
        y = pad + r * (cell_h + label_h + pad)
        sheet.paste(canvas.resize((cell_w, cell_h), Image.Resampling.NEAREST), (x, y))
        tw = draw.textlength(label, font=font)
        draw.text((x + (cell_w - tw) / 2, y + cell_h + 4), label, fill=(28, 25, 22), font=font)
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(PREVIEW)
    print(f"wrote {PREVIEW}")


if __name__ == "__main__":
    main()
