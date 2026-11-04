"""Checks a harness recording. The white bar moves 1 pixel per rendered frame, so dropped or repeated frames, streams that
got mixed up, and any trace of the menu in the video all show up here.

    check_video.py <file> [--step N] [--start N] [--frames N] [--soft-edge] [--no-bar]
"""
import argparse
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("path")
parser.add_argument("--step", type=int, default=1, help="how far the bar should move per video frame")
parser.add_argument("--frames", type=int, default=0, help="how many video frames there should be")
parser.add_argument("--soft-edge", action="store_true", help="motion blur: the bar's edges should be in-between greys")
parser.add_argument("--no-bar", action="store_true", help="only count frames")
args = parser.parse_args()

width, height = 1920, 1080
probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", args.path],
                       capture_output=True, text=True, check=True).stdout.strip().split(",")
width, height = int(probe[0]), int(probe[1])
raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", args.path, "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                     capture_output=True, check=True).stdout
frames = len(raw) // (width * height)
print(f"frames: {frames}", end="")
ok = frames > 0 and (args.frames == 0 or frames == args.frames)

if not args.no_bar:
    bar_row = int(height * 0.5)
    menu_row = int(height * 0.14)
    positions, soft, menu_pixels = [], 0, 0
    for index in range(frames):
        frame = raw[index * width * height : (index + 1) * width * height]
        row = frame[bar_row * width : (bar_row + 1) * width]
        bright = [x for x in range(width) if row[x] > 200]
        positions.append(bright[0] if bright else None)
        background = row[width - 1]
        if any(60 < row[x] < 190 and abs(int(row[x]) - int(background)) > 20 for x in range(width)):
            soft += 1
        top = frame[menu_row * width : (menu_row + 1) * width]
        menu_pixels += sum(1 for x in range(width) if abs(int(top[x]) - int(top[width - 1])) > 12)
    steps = [b - a for a, b in zip(positions, positions[1:]) if a is not None and b is not None]
    bad = [s for s in steps if s != args.step]
    print(f"; bar x={positions[0]}..{positions[-1]}; wrong steps: {len(bad)} {bad[:6]}; menu pixels: {menu_pixels}", end="")
    ok = ok and not bad and menu_pixels == 0
    if args.soft_edge:
        print(f"; frames with blurred edges: {soft}", end="")
        ok = ok and soft >= frames * 0.8

print("; PASS" if ok else "; FAIL")
sys.exit(0 if ok else 1)
