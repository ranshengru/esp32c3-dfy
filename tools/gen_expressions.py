#!/usr/bin/env python3
"""Pack eight expression clips, 24 frames each.

The screen is already 128x160, so resolution is not a lever. Frames stay
baseline 4:2:0 JPEG (tjpgd rejects progressive). Quality is ffmpeg -q:v 5:
q:v 8 and worse blocks the eyes, and a denoise pass does not shrink this art.
24 frames cover the original clip. A blink peak replaces the nearest even
sample so a closed eye is not skipped, and the count stays 24.
"""

import struct
import subprocess
import zlib
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "assets" / "expressions"
BIN_PATH = ROOT / "firmware" / "idf" / "assets.bin"
PREVIEW = ROOT / "docs" / "v1.0-expressions-preview.png"
SHEET = ROOT / "docs" / "v1.0-24frames.png"
WIDTH, HEIGHT = 128, 160
SHIFT = 10
PACK_VERSION = 20261116
VIDEO_DIR = ROOT / "assets" / "expressions" / "videos"
FRAME_COUNT = 24
SRC_FPS = 24
QV = 5

EXPRS = [
    ("neutral", "平常", "neutral.mp4"),
    ("happy", "开心", "happy.mp4"),
    ("sleepy", "困", "sleepy.mp4"),
    ("sleeping", "打盹", "sleeping.mp4"),
    ("tsundere", "傲娇", "tsundere.mp4"),
    ("surprised", "惊讶", "surprised.mp4"),
    ("worried", "担心", "worried.mp4"),
    ("excited", "兴奋", "excited.mp4"),
    ("rice", "吃饭", "rice.mp4"),
]


def tight_crop(canvas):
    """Drop the blank rows above the character. Feet stay on the bottom row,
    so a shorter frame sits at the bottom of the 160-tall screen.
    """
    pixels = canvas.load()
    top = HEIGHT
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = pixels[x, y]
            if r < 242 or g < 242 or b < 242:
                top = y
                break
        if top != HEIGHT:
            break
    limit = max(0, top - 2)
    y0 = 0
    for candidate in (64, 48, 32, 16):
        if candidate <= limit:
            y0 = candidate
            break
    if y0 == 0:
        return canvas, HEIGHT
    return canvas.crop((0, y0, WIDTH, HEIGHT)), HEIGHT - y0


def screen_canvas(im):
    art = ImageOps.contain(ImageOps.exif_transpose(im).convert("RGB"), (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(art, ((WIDTH - art.width) // 2, (HEIGHT - art.height) // 2 + SHIFT))
    return canvas


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def is_baseline(data):
    i = 2
    while i + 3 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker == 0xC0:
            return True
        if marker in (0xC1, 0xC2, 0xC3):
            return False
        if marker == 0xD8 or marker == 0xD9 or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        if marker == 0x00 or marker == 0xFF:
            i += 1
            continue
        seglen = int.from_bytes(data[i + 2 : i + 4], "big")
        if seglen < 2:
            return False
        i += 2 + seglen
    return False


def dump_samples(video, dest):
    if dest.exists():
        subprocess.check_call(["rm", "-rf", str(dest)])
    dest.mkdir(parents=True)
    subprocess.check_call(
        [
            "ffmpeg", "-y", "-i", str(video),
            "-vf", f"fps={SRC_FPS},scale=96:-2",
            "-q:v", "10", str(dest / "f_%03d.jpg"),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return sorted(dest.glob("f_*.jpg"))


def mean_diff(a, b):
    d = ImageChops.difference(a, b)
    hist = d.histogram()
    return sum(i * c for i, c in enumerate(hist)) / sum(hist)


def pick_indices(frames):
    grays = [Image.open(p).convert("L") for p in frames]
    n = len(grays)
    diffs = [0.0]
    for i in range(1, n):
        diffs.append(mean_diff(grays[i], grays[i - 1]))
    grid = [round(i * (n - 1) / (FRAME_COUNT - 1)) for i in range(FRAME_COUNT)]
    peaks = [
        i
        for i in range(1, n - 1)
        if diffs[i] >= diffs[i - 1] and diffs[i] >= diffs[i + 1] and diffs[i] >= 6
    ]
    peaks.sort(key=lambda i: diffs[i], reverse=True)
    chosen = set(grid)
    for peak in peaks[:10]:
        if peak in chosen:
            continue
        nearest = min(chosen, key=lambda g: abs(g - peak))
        if abs(nearest - peak) <= 4 and diffs[nearest] + 4 < diffs[peak]:
            chosen.remove(nearest)
            chosen.add(peak)
    ordered = sorted(chosen)
    while len(ordered) > FRAME_COUNT:
        drop = min(ordered[1:-1], key=lambda i: diffs[i])
        ordered.remove(drop)
    return ordered, n


def grab_frames(video, indices, dest_dir):
    """One decode of the source. select= keeps the chosen frames in order."""
    if dest_dir.exists():
        subprocess.check_call(["rm", "-rf", str(dest_dir)])
    dest_dir.mkdir(parents=True)
    choose = "+".join(f"eq(n\\,{index})" for index in indices)
    subprocess.check_call(
        [
            "ffmpeg", "-y", "-i", str(video),
            "-vf", f"select='{choose}',setpts=N/FRAME_RATE/TB",
            "-vsync", "vfr", str(dest_dir / "f_%02d.png"),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    frames = sorted(dest_dir.glob("f_*.png"))
    if len(frames) != len(indices):
        raise SystemExit(f"{video.name}: grabbed {len(frames)} frames, wanted {len(indices)}")
    return frames


def encode_jpeg(canvas, dest):
    """Same encoder as the other clips: one baseline 4:2:0 JPEG, ffmpeg -q:v 5."""
    png = dest.with_suffix(".png")
    canvas.save(png)
    subprocess.check_call(
        [
            "ffmpeg", "-y", "-i", str(png),
            "-c:v", "mjpeg", "-q:v", str(QV), "-pix_fmt", "yuvj420p",
            "-frames:v", "1", str(dest),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    png.unlink()
    data = dest.read_bytes()
    dest.unlink()
    if not data.startswith(b"\xff\xd8") or not is_baseline(data) or len(data) > 20 * 1024:
        raise SystemExit(f"bad jpeg ({len(data)} bytes)")
    return data


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("*_[0-9]*.png"):
        old.unlink()
    entries = []
    previews = []
    strips = []
    sample_root = Path("/tmp/expr-samples")
    for expr_id, (key, label, video_name) in enumerate(EXPRS):
        video = VIDEO_DIR / video_name
        samples = dump_samples(video, sample_root / key)
        indices, total = pick_indices(samples)
        duration = total / SRC_FPS
        times = [i / SRC_FPS for i in indices]
        print(f"{key}: {total} src frames -> {indices}")
        grabbed = grab_frames(video, indices, Path(f"/tmp/grab-{key}"))
        canvases = []
        heights = []
        for full in grabbed:
            canvas = screen_canvas(Image.open(full))
            cropped, frame_h = tight_crop(canvas)
            canvases.append((canvas, cropped))
            heights.append(frame_h)
        row = []
        for frame_i, (index, t) in enumerate(zip(indices, times)):
            if frame_i + 1 < len(times):
                nxt = times[frame_i + 1]
            elif frame_i:
                nxt = t + (t - times[frame_i - 1])
            else:
                nxt = t + 1 / SRC_FPS
            hold_ms = max(40, int(round((nxt - t) * 1000)))
            canvas, cropped = canvases[frame_i]
            jpeg = encode_jpeg(cropped, Path(f"/tmp/enc-{key}-{frame_i}.jpg"))
            entries.append((expr_id, frame_i, hold_ms, jpeg))
            if frame_i == 0:
                previews.append((label, canvas))
            row.append(canvas)
            print(f"  [{frame_i:02d}] src {index} hold {hold_ms}ms h {heights[frame_i]} jpeg {len(jpeg)}")
        subprocess.check_call(["rm", "-rf", f"/tmp/grab-{key}"])
        strips.append((label, row))

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

    scale = 2
    cols = 5
    rows = (len(previews) + cols - 1) // cols
    pad = 12
    label_h = 26
    cell_w, cell_h = WIDTH * scale, HEIGHT * scale
    sheet = Image.new(
        "RGB",
        (cols * cell_w + (cols + 1) * pad, rows * (cell_h + label_h) + (rows + 1) * pad),
        (243, 234, 220),
    )
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 18)
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

    thumb_w, thumb_h = 64, 80
    film = Image.new("RGB", (pad + FRAME_COUNT * thumb_w, pad + len(strips) * (thumb_h + 22)), (243, 234, 220))
    pen = ImageDraw.Draw(film)
    for r, (label, row) in enumerate(strips):
        y = pad + r * (thumb_h + 22)
        pen.text((4, y), label, fill=(40, 32, 28), font=font)
        for i, canvas in enumerate(row):
            film.paste(canvas.resize((thumb_w, thumb_h), Image.Resampling.NEAREST), (i * thumb_w, y + 20))
    film.save(SHEET)
    print(f"wrote {PREVIEW} and {SHEET}")


if __name__ == "__main__":
    main()
