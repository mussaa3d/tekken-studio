"""Turns offsets in the mod's DLL (from a crash report's call stack) into function names and lines, using the build's PDB.

    python tools/symbolize.py 3a3cf 3b704
"""
import ctypes
import ctypes.wintypes as w
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = os.path.join(ROOT, "build", "dinput8.dll")
BASE = 0x10000000


class Symbol(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", w.ULONG), ("Size", w.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", w.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", w.ULONG),
                ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG), ("MaxNameLen", w.ULONG), ("Name", ctypes.c_char * 512)]


class Line(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", w.DWORD), ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]


def main():
    help_dll = ctypes.WinDLL("dbghelp")
    process = ctypes.windll.kernel32.GetCurrentProcess()
    help_dll.SymInitialize.argtypes = [w.HANDLE, ctypes.c_char_p, w.BOOL]
    help_dll.SymLoadModuleEx.restype = ctypes.c_uint64
    help_dll.SymLoadModuleEx.argtypes = [w.HANDLE, w.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, w.DWORD, ctypes.c_void_p, w.DWORD]
    help_dll.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(Symbol)]
    help_dll.SymGetLineFromAddr64.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(w.DWORD), ctypes.POINTER(Line)]
    help_dll.SymSetOptions(0x2 | 0x10)  # undecorated names, load lines
    help_dll.SymInitialize(process, os.path.dirname(DLL).encode(), False)
    if not help_dll.SymLoadModuleEx(process, None, DLL.encode(), None, BASE, 0, None, 0):
        sys.exit("could not load symbols")
    for text in sys.argv[1:]:
        address = BASE + int(text, 16)
        symbol = Symbol()
        symbol.SizeOfStruct = ctypes.sizeof(Symbol) - 511
        symbol.MaxNameLen = 512
        displacement = ctypes.c_uint64()
        name = "?"
        if help_dll.SymFromAddr(process, address, ctypes.byref(displacement), ctypes.byref(symbol)):
            name = f"{symbol.Name.decode()}+0x{displacement.value:x}"
        line = Line()
        line.SizeOfStruct = ctypes.sizeof(Line)
        offset = w.DWORD()
        where = ""
        if help_dll.SymGetLineFromAddr64(process, address, ctypes.byref(offset), ctypes.byref(line)):
            where = f"  {os.path.basename(line.FileName.decode())}:{line.LineNumber}"
        print(f"{text:>8}  {name}{where}")


if __name__ == "__main__":
    main()
