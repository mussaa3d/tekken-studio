"""Looks through the game's executable on this PC: finds byte patterns and disassembles around an address.

    python tools/disasm.py find "41 B8 58 02 00 00 44 39 40 1C"     every place the pattern is (RVAs)
    python tools/disasm.py at 0x2E15940 [--count 80] [--before 32]  disassemble from an RVA
    python tools/disasm.py callers 0x2E15940                        every direct call or jump to an RVA
    python tools/disasm.py rip 0x9B659E0                            every instruction that addresses an RVA relative to RIP
    python tools/disasm.py strings "Rewind"                         UTF-16 and ASCII strings containing the text, with RVAs
"""
import argparse
import re
import struct
from pathlib import Path

import capstone
import pefile

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\TEKKEN 8\Polaris\Binaries\Win64\Polaris-Win64-Shipping.exe")


class Image:
    def __init__(self):
        self.pe = pefile.PE(str(EXE), fast_load=True)
        self.data = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.text = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".text")
        self.text_start = self.text.VirtualAddress
        self.text_end = self.text_start + self.text.Misc_VirtualSize
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = False

    def disasm(self, rva, count):
        return list(self.md.disasm(self.data[rva : rva + count * 16], rva, count))


def to_regex(pattern):
    parts = []
    for token in pattern.split():
        parts.append(b"." if token in ("?", "??") else re.escape(bytes([int(token, 16)])))
    return re.compile(b"".join(parts), re.S)


def show(image, rva, count):
    for ins in image.disasm(rva, count):
        print(f"{ins.address:#010x}  {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("what", choices=["find", "at", "callers", "rip", "strings"])
    parser.add_argument("value")
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--before", type=int, default=0)
    args = parser.parse_args()
    image = Image()

    if args.what == "find":
        for match in to_regex(args.value).finditer(image.data):
            print(f"{match.start():#010x}")
    elif args.what == "at":
        show(image, int(args.value, 16) - args.before, args.count)
    elif args.what == "callers":
        target = int(args.value, 16)
        text = image.data[image.text_start : image.text_end]
        for opcode in (0xE8, 0xE9):
            for match in re.finditer(re.escape(bytes([opcode])), text, re.S):
                offset = match.start()
                if offset + 5 > len(text):
                    continue
                displacement = struct.unpack_from("<i", text, offset + 1)[0]
                if image.text_start + offset + 5 + displacement == target:
                    print(f"{image.text_start + offset:#010x} {'call' if opcode == 0xE8 else 'jmp'}")
    elif args.what == "rip":
        target = int(args.value, 16)
        text = image.data[image.text_start : image.text_end]
        # A RIP-relative disp32 ends the instruction, or is followed by a small immediate.
        for tail in (0, 1, 4):
            for offset in range(len(text) - 4 - tail):
                displacement = struct.unpack_from("<i", text, offset)[0]
                if image.text_start + offset + 4 + tail + displacement != target:
                    continue
                start = image.text_start + offset - 3
                for lead in (2, 3, 4):
                    candidate = image.text_start + offset - lead
                    decoded = image.disasm(candidate, 1)
                    if decoded and decoded[0].size == lead + 4 + tail and "rip" in decoded[0].op_str:
                        print(f"{decoded[0].address:#010x}  {decoded[0].mnemonic} {decoded[0].op_str}")
                        break
    elif args.what == "strings":
        text = args.value
        for needle, kind in ((text.encode("utf-16-le"), "utf16"), (text.encode(), "ascii")):
            for match in re.finditer(re.escape(needle), image.data):
                start = match.start()
                step = 2 if kind == "utf16" else 1
                while start >= step and image.data[start - step] != 0:
                    start -= step
                end = match.end()
                while end < len(image.data) and image.data[end] != 0:
                    end += step
                raw = image.data[start:end]
                print(f"{start:#010x} {kind} {raw.decode('utf-16-le' if kind == 'utf16' else 'latin1', 'replace')}")


if __name__ == "__main__":
    main()
