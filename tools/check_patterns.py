"""Checks the mod's code patterns against the game's exe without running the game, and whether the exe imports
DINPUT8 (which is how the mod gets loaded). Patterns come straight from src/mod/game.cpp.

    python tools/check_patterns.py "<path to Polaris-Win64-Shipping.exe>"

A protected exe can keep its code encrypted on disk, so a miss here isn't final; the game log has the last word.
"""
import re
import struct
import sys
from pathlib import Path


def load_patterns():
    source = (Path(__file__).parent.parent / "src" / "mod" / "game.cpp").read_text(encoding="utf-8")
    entries = re.findall(r'\{ "(\w+)", "([0-9A-F? ]+)", (\d+), (true|false), (true|false), &Addresses::', source)
    return [(name, pattern, name_required == "true") for name, pattern, _, _, name_required in entries]


def to_regex(pattern):
    parts = [b"." if token in ("?", "??") else re.escape(bytes([int(token, 16)])) for token in pattern.split()]
    return re.compile(b"".join(parts), re.DOTALL)


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional
    for index in range(count):
        at = table + index * 40
        name = data[at : at + 8].rstrip(b"\0").decode("latin1")
        size, offset = struct.unpack_from("<II", data, at + 16)
        yield name, offset, size


def imports_dinput8(data):
    return re.search(rb"dinput8\.dll", data, re.IGNORECASE) is not None


def main():
    path = Path(sys.argv[1])
    data = path.read_bytes()
    stamp = struct.unpack_from("<I", data, struct.unpack_from("<I", data, 0x3C)[0] + 8)[0]
    print(f"{path.name}: {len(data) / 1048576:.0f} MB, TimeDateStamp {stamp}")
    print("sections:", ", ".join(f"{name} ({size // 1048576} MB)" for name, _, size in sections(data)))
    print("mentions dinput8.dll:", imports_dinput8(data))

    problems = 0
    for name, pattern, required in load_patterns():
        hits = sum(1 for _ in to_regex(pattern).finditer(data))
        state = "ok" if hits == 1 else ("MISSING" if hits == 0 else f"{hits} MATCHES")
        if hits != 1 and required:
            problems += 1
        print(f"  {name:30} {state}{'' if required else '  (optional)'}")
    print("all required patterns found" if problems == 0 else f"{problems} required pattern(s) need attention")
    return 0 if problems == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
