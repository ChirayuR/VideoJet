#!/usr/bin/env python3
"""Synthetic Android-Auto-like H.264 test streams for the VideoJet bench.

AA's second screen arrives as 800x480 H.264 baseline at 30 fps, with the panel-sized
picture (640x172) top-left and blank margins (the Android Auto second-screen profile). These mimic it.
Usage: python3 tools/gen_streams.py   -> streams/search/*, streams/heldout/* (+ .golden each)
"""
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
W, H, PW, PH, FPS, N = 800, 480, 640, 172, 30, 300
NAMES = ["MG Rd", "Hosur Rd", "Outer Ring Rd", "Sarjapur Rd", "Bannerghatta Rd", "Old Airport Rd"]
MAP = 3000


def font(size):
    return ImageFont.load_default(size=size)


def route(t):
    """Camera path: a gentle S-curve down the map, t in [0, 1]."""
    return MAP / 2 + MAP * 0.15 * math.sin(t * 2 * math.pi), MAP * (0.2 + 0.6 * t)


def make_map(seed, dark):
    rng = np.random.default_rng(seed)
    if dark:
        bg, block, minor, major, label = (30, 34, 42), (38, 44, 54), (62, 68, 80), (96, 104, 118), (190, 190, 190)
    else:
        bg, block, minor, major, label = (236, 232, 224), (222, 218, 208), (255, 255, 255), (250, 214, 130), (70, 70, 70)
    im = Image.new("RGB", (MAP, MAP), bg)
    d = ImageDraw.Draw(im)
    for _ in range(400):
        x, y = (int(v) for v in rng.integers(0, MAP, 2))
        w, h = (int(v) for v in rng.integers(40, 260, 2))
        d.rectangle([x, y, x + w, y + h], fill=block)
    for i in range(0, MAP, 110):
        j = int(rng.integers(-30, 30))
        wd = int(rng.choice([5, 5, 9, 15]))
        col = major if wd > 9 else minor
        d.line([(i + j, 0), (i - j, MAP)], fill=col, width=wd)
        d.line([(0, i + j), (MAP, i - j)], fill=col, width=wd)
    for _ in range(10):
        a = [int(v) for v in rng.integers(0, MAP, 4)]
        d.line([(a[0], a[1]), (a[2], a[3])], fill=major, width=18)
    f = font(20)
    for _ in range(260):
        x, y = (int(v) for v in rng.integers(0, MAP - 200, 2))
        d.text((x, y), NAMES[int(rng.integers(0, len(NAMES)))], fill=label, font=f)
    d.line([route(i / 200) for i in range(201)], fill=(66, 133, 244), width=14)  # AA-blue route
    return im


def map_frames(seed, dark, vw, vh):
    """Heading-up camera following the route, ~3 px/frame, like AA's map while riding."""
    m = make_map(seed, dark)
    ink = (255, 255, 255) if dark else (20, 20, 20)
    ox, oy = vw / 2, vh * 0.7  # camera anchor inside the view
    for i in range(N):
        t = i / N * 0.35
        cx, cy = route(t)
        nx, ny = route(t + 0.002)
        th = math.atan2(nx - cx, -(ny - cy))
        c, s = math.cos(th), math.sin(th)
        # output (x, y) -> map (c*x - s*y + e, s*x + c*y + f): rotation about the anchor
        e = cx - (c * ox - s * oy)
        f = cy - (s * ox + c * oy)
        view = m.transform((vw, vh), Image.Transform.AFFINE, (c, -s, e, s, c, f),
                           resample=Image.Resampling.BILINEAR)
        vd = ImageDraw.Draw(view)
        vd.polygon([(ox, oy - 14), (ox - 10, oy + 10), (ox + 10, oy + 10)], fill=(255, 255, 255), outline=(20, 20, 20))
        vd.text((8, 6), "12:34", fill=ink, font=font(18))
        yield view


def card_frames():
    """Turn card: static except the distance (every 0.5 s), a progress bar, and a maneuver flip at 5 s."""
    for i in range(N):
        im = Image.new("RGB", (PW, PH), (24, 28, 34))
        d = ImageDraw.Draw(im)
        # right-turn arrow; from 5 s it is mirrored into a left turn (x -> 140 - x)
        arrow = [(40, 140), (40, 52), (77, 52), (77, 36), (99, 66), (77, 96), (77, 80), (68, 80), (68, 140)]
        if (i // 150) % 2:
            arrow = [(140 - x, y) for x, y in arrow]
        d.polygon(arrow, fill=(255, 255, 255))
        d.text((150, 22), f"{max(0, 350 - (i // 15) * 10)} m", fill=(255, 255, 255), font=font(64))
        d.text((150, 108), NAMES[(i // 150) % len(NAMES)], fill=(170, 180, 190), font=font(30))
        d.rectangle([150, 155, 150 + int(450 * (1 - i / N)), 162], fill=(66, 133, 244))
        yield im


def boxed(views):
    """Panel picture at the top-left of an 800x480 frame, the rest black (Android Auto second-screen layout)."""
    for v in views:
        frame = Image.new("RGB", (W, H), (0, 0, 0))
        frame.paste(v, (0, 0))
        yield frame


def to_i420(rgb):
    a = np.asarray(rgb, dtype=np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    y = 16 + 0.257 * r + 0.504 * g + 0.098 * b
    u = 128 - 0.148 * r - 0.291 * g + 0.439 * b
    v = 128 + 0.439 * r - 0.368 * g - 0.071 * b

    def sub(p):
        return p.reshape(p.shape[0] // 2, 2, p.shape[1] // 2, 2).mean(axis=(1, 3))

    def q(p):
        return np.clip(np.rint(p), 0, 255).astype(np.uint8).tobytes()

    return q(y) + q(sub(u)) + q(sub(v))


def encode(frames, out, kbps, extra=()):
    """x264 tuned like a phone's HW encoder for AA: constrained baseline, 1 ref, one IDR, CBR."""
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["x264", "--demuxer", "y4m", "--profile", "baseline", "--level", "3.1", "--preset", "veryfast",
           "--tune", "zerolatency", "--threads", "1", "--keyint", "infinite",
           "--scenecut", "0", "--ref", "1", "--bframes", "0", "--bitrate", str(kbps),
           "--vbv-maxrate", str(kbps), "--vbv-bufsize", str(kbps), "--colormatrix", "bt470bg",
           "--range", "tv", "--log-level", "warning", "--no-progress", "--cpu-independent",
           *extra, "-o", str(out), "-"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    try:
        p.stdin.write(f"YUV4MPEG2 W{W} H{H} F{FPS}:1 Ip A1:1 C420jpeg\n".encode())
        for fr in frames:
            p.stdin.write(b"FRAME\n" + to_i420(fr))
        p.stdin.close()
    except BrokenPipeError:
        pass  # x264 exited early; its status is checked below
    if p.wait():
        sys.exit(f"x264 failed for {out}")
    subprocess.run([str(ROOT / "tools" / "golden.sh"), str(out), str(W), str(H)], check=True)
    check_golden(out)


def check_golden(out):
    """The golden must hold one hash per frame: N lines, or the run fails."""
    gold = out.with_suffix(".golden")
    n = len(gold.read_text().splitlines())
    if n != N:
        sys.exit(f"{gold}: {n} golden lines, expected {N}")


def main():
    s, h = ROOT / "streams" / "search", ROOT / "streams" / "heldout"
    encode(boxed(map_frames(1, False, PW, PH)), s / "aux_map.h264", 2000)
    encode(boxed(card_frames()), s / "aux_card.h264", 800)
    encode(map_frames(1, False, W, H), s / "full_map.h264", 3000)
    encode(boxed(map_frames(2, True, PW, PH)), h / "aux_map_dark.h264", 2500, ("--preset", "faster", "--slices", "4"))


if __name__ == "__main__":
    main()
