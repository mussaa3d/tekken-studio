# Research helper: who calls a recompiled function, up the chain, from the play test's generated sources.
#   python callers.py <playtest folder> <sub_XXXXXXXX> [depth] [file of names BlueSpark's library already replaces]
# Prints each level's callers; a name with * is one the library replaces (so the studio can't hook it).
import glob
import os
import re
import sys

playtest, target = sys.argv[1], sys.argv[2]
depth = int(sys.argv[3]) if len(sys.argv) > 3 else 6
hooked = set(open(sys.argv[4]).read().split()) if len(sys.argv) > 4 else set()

callers = {}  # callee -> set of callers
define = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
call = re.compile(r"\b(sub_[0-9A-F]{8})\(ctx, base\)")
for path in glob.glob(os.path.join(playtest, "rex", "generated", "default", "ttt2_recomp.*.cpp")):
    current = None
    with open(path, encoding="utf-8", errors="replace") as file:
        for line in file:
            m = define.match(line)
            if m:
                current = m.group(1)
                continue
            if current:
                for callee in call.findall(line):
                    callers.setdefault(callee, set()).add(current)

level = {target}
seen = {target}
for d in range(1, depth + 1):
    above = set()
    for name in level:
        above |= callers.get(name, set())
    above -= seen
    if not above:
        print(f"level {d}: none (called through a pointer or a table from here up)")
        break
    seen |= above
    print(f"level {d}: " + " ".join(sorted(n + ("*" if n in hooked else "") for n in above)))
    level = above
