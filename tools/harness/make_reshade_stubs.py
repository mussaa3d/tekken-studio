"""Writes fake_reshade_stubs.inc: do-nothing versions of every method of ReShade's effect_runtime, device and command_queue.

The stand-in ReShade the harness loads (fake_reshade.cpp) is built on these, so its objects have the same layout as the real ones in
the ReShade headers the mod is built against. Run it again after updating third_party/reshade:

    python tools/harness/make_reshade_stubs.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
include = root / "third_party" / "reshade" / "include"
text = {name: (include / name).read_text(encoding="utf-8") for name in ("reshade_api.hpp", "reshade_api_device.hpp")}


def methods(header, struct):
    source = text[header]
    start = re.search(r"struct __declspec\(novtable\) " + struct + r"\b[^{]*\{", source).end()
    body = source[start:source.index("\n\t};", start)]
    found = []
    for line in re.findall(r"^\t\tvirtual ([^\n]*?) = 0;", body, re.M):
        match = re.match(r"(?P<ret>.+?)(?P<name>\w+)\((?P<args>.*)\)(?P<const> const)?$", line.strip())
        if not match:
            raise SystemExit(f"can't read: {line}")
        found.append(match)
    return found


def stub(name, base, chain):
    lines = [f"struct {name} : {base} {{"]
    for header, struct in chain:
        lines.append(f"    // {struct}")
        for m in methods(header, struct):
            returns = m["ret"].strip()
            body = "{}" if returns == "void" else "{ return {}; }"
            lines.append(f"    {m['ret']}{m['name']}({m['args']}){m['const'] or ''} override {body}")
    lines.append("};")
    return "\n".join(lines)


out = [
    "// Made by make_reshade_stubs.py from the ReShade headers in third_party/reshade. Don't edit: run the script again.",
    stub("StubRuntime", "effect_runtime", [("reshade_api_device.hpp", "api_object"), ("reshade_api_device.hpp", "device_object"), ("reshade_api.hpp", "effect_runtime")]),
    stub("StubDevice", "device", [("reshade_api_device.hpp", "api_object"), ("reshade_api_device.hpp", "device")]),
    stub("StubQueue", "command_queue", [("reshade_api_device.hpp", "api_object"), ("reshade_api_device.hpp", "device_object"), ("reshade_api_device.hpp", "command_queue")]),
    "",
]
target = Path(__file__).with_name("fake_reshade_stubs.inc")
target.write_text("\n\n".join(out), encoding="utf-8", newline="\n")
print(f"wrote {target.name}: {sum(part.count(' override ') for part in out)} methods")
