"""Builds a contact sheet from recordings: one picture from each given video at the given frame number.

    python tools/sheet.py out.png 45 a.mp4 b.mp4 c.mp4
"""
import subprocess
import sys


def main():
    out, frame, videos = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    args = ["ffmpeg", "-v", "error", "-y"]
    for video in videos:
        args += ["-i", video]
    graph = ""
    for i in range(len(videos)):
        graph += f"[{i}:v]select='eq(n,{frame})',scale=640:-1[v{i}];"
    columns = 2 if len(videos) <= 4 else 3
    layout = "|".join(f"{(i % columns) * 640}_{(i // columns) * 360}" for i in range(len(videos)))
    graph += "".join(f"[v{i}]" for i in range(len(videos))) + (f"xstack=inputs={len(videos)}:layout={layout}[out]" if len(videos) > 1 else "null[out]")
    subprocess.run(args + ["-filter_complex", graph, "-map", "[out]", "-frames:v", "1", out], check=True)


if __name__ == "__main__":
    main()
