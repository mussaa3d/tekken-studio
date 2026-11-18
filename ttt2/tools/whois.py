"""Research helper: names the function an instruction offset in ttt2-studio.exe is in, from the linker's map.
Usage: whois.py <map file> <hex offset>..."""
import bisect, re, sys

path, targets = sys.argv[1], [int(t, 16) for t in sys.argv[2:]]
symbols = []
with open(path, encoding='latin-1') as f:
    for line in f:
        # MSVC-style map: " 0001:0000abcd  name  00000001400xxxxx  f  object"
        m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s', line)
        if m:
            symbols.append((int(m.group(2), 16), m.group(1)))
symbols.sort()
base = 0x140000000
addresses = [a for a, _ in symbols]
for t in targets:
    i = bisect.bisect_right(addresses, base + t) - 1
    if i < 0:
        print(f'{t:x}: before the first symbol')
        continue
    a, name = symbols[i]
    print(f'{t:x}: {name} + 0x{base + t - a:x}')
