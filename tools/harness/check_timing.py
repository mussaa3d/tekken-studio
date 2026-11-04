"""Checks frame-locked recording with the real tick code. In the live scene the white bar moves 240 pixels per second of game
clock, so at a given timescale and framerate each video frame must move it by exactly 240 * speed / fps pixels.

    check_timing.py <file> <speed> <fps>
"""
import subprocess
import sys

path, speed, fps = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
width, height = map(int, subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                                        capture_output=True, text=True, check=True).stdout.strip().split(","))
raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, check=True).stdout
frames = len(raw) // (width * height)
row = height - 90
positions = []
for index in range(frames):
    line = raw[index * width * height + row * width : index * width * height + (row + 1) * width]
    bright = [x for x in range(width) if line[x] > 200]
    positions.append(bright[0] if bright else None)
steps = [b - a for a, b in zip(positions, positions[1:]) if a is not None and b is not None]
expected = 240 * speed / fps
# Total distance over total frames: at slow speeds the bar moves less than a pixel per frame, so single steps are 0 or 1.
average = sum(steps) / len(steps) if steps else 0
spread = max(steps) - min(steps) if steps else 0
ok = abs(average - expected) < 0.05 and spread <= 1 and min(steps, default=-1) >= 0
print(f"frames {frames}; bar moves {average:.3f} px per frame, expected {expected:.3f}; spread {spread}; {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
