#!/usr/bin/env python3
"""Pack expression clips from the original videos.

Keyframes are picked from each clip, not sampled on a fixed clock. A stretch
where the picture does not change is not stored. The first frame is the
representative pose and stays up longer; a blink or a drifting Z is short.
"""

import io
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
PACK_VERSION = 20261113
VIDEO_DIR = ROOT / "assets" / "expressions" / "videos"

# (name, label, source video, [(frame_index at 8 fps, hold_ms), ...])
# Picked by watching the original clip. A repeated blink, a return to the same
# pose, or a whole-body wobble with the same face is not a keyframe.
EXPRS = [
    ("neutral", "平常", "neutral.mp4", [(4, 2200), (28, 160)]),
    ("happy", "开心", "happy.mp4", [(1, 1800), (7, 180)]),
    ("sleepy", "困", "sleepy.mp4", [(1, 1200), (8, 250), (16, 1400)]),
    ("sleeping", "打盹", "sleeping.mp4", [(0, 800), (2, 400), (10, 500)]),
    ("tsundere", "傲娇", "tsundere.mp4", [(4, 2000), (34, 220)]),
    ("surprised", "惊讶", "surprised.mp4", [(4, 2000), (28, 180)]),
    ("worried", "担心", "worried.mp4", [(0, 1800), (6, 200)]),
    ("excited", "兴奋", "excited.mp4", [(12, 1600), (6, 320)]),
]


def rgb565(color):
    r, g, b = color
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def screen_canvas(im):
    art = ImageOps.contain(ImageOps.exif_transpose(im).convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(art, ((WIDTH - art.width) // 2, (HEIGHT - art.height) // 2 + SHIFT))
    return canvas


def grab(video, index, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    # 0-based index into an 8 fps sampling of the original clip.
    subprocess.check_call(
        [
            "ffmpeg", "-y", "-i", str(video),
            "-vf", f"fps=8,select=eq(n\\,{index})",
            "-vsync", "vfr", "-frames:v", "1", str(dest),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def frame_bytes(canvas):
    if canvas.size != (WIDTH, HEIGHT):
        raise SystemExit(f"frame is {canvas.size}, expected {(WIDTH, HEIGHT)}")
    buf = io.BytesIO()
    canvas.convert("RGB").save(buf, "JPEG", quality=90, optimize=True, subsampling=2, progressive=False)
    data = buf.getvalue()
    if not data.startswith(b"\xff\xd8") or len(data) > 20 * 1024:
        raise SystemExit(f"bad jpeg ({len(data)} bytes)")
    return data


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    entries = []
    previews = []
    for expr_id, (key, label, video_name, keys) in enumerate(EXPRS):
        kept = []
        for frame_i, (index, hold_ms) in enumerate(keys):
            full = OUT_DIR / f".grab_{key}_{frame_i}.png"
            grab(VIDEO_DIR / video_name, index, full)
            canvas = screen_canvas(Image.open(full))
            full.unlink()
            canvas.save(OUT_DIR / f"{key}_{frame_i}.png")
            jpeg = frame_bytes(canvas)
            kept.append((expr_id, frame_i, hold_ms, jpeg))
            if frame_i == 0:
                previews.append((label, canvas))
            print(f"{key} frame@{index} hold {hold_ms}ms: jpeg {len(jpeg)}")
        stale = frame_i
        while True:
            stale += 1
            old = OUT_DIR / f"{key}_{stale}.png"
            if not old.exists():
                break
            old.unlink()
        entries.extend(kept)

    directory = bytearray()
    payload = bytearray()
    header_size = 32
    cursor = header_size + len(entries) * 16
    for expr_id, frame_i, hold_ms, comp in entries:
        directory += struct.pack("<BBHIII", expr_id, frame_i, hold_ms, cursor, len(comp), crc32(comp))
        payload += comp
        cursor += len(comp)
    data = bytes(directory) + bytes(payload)
    head = struct.pack(
        "<4sHHIHHBBHII",
        b"DFYA",
        4,
        header_size,
        PACK_VERSION,
        WIDTH,
        HEIGHT,
        2,
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
