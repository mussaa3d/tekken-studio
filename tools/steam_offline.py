"""Turns Steam's Offline Mode on or off for the signed-in account, for testing. Steam must be closed, since it rewrites this
file on exit. The first run keeps a copy of the file next to it.

    python tools/steam_offline.py on|off|status
"""
import re
import shutil
import sys
import winreg
from pathlib import Path

path = Path(r"C:\Program Files (x86)\Steam\config\loginusers.vdf")
text = path.read_text(encoding="utf-8")
blocks = list(re.finditer(r'"\d{17}"\s*\{.*?\}', text, re.DOTALL))
with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
    auto_login = winreg.QueryValueEx(key, "AutoLoginUser")[0]
current = next((m for m in blocks if re.search(rf'"AccountName"\s*"{re.escape(auto_login)}"', m.group(0), re.IGNORECASE)), None)
if current is None:
    sys.exit("no signed-in account found")

command = sys.argv[1]
if command == "status":
    print("offline" if re.search(r'"WantsOfflineMode"\s*"1"', current.group(0)) else "online")
    sys.exit(0)

value = "1" if command == "on" else "0"
backup = path.with_suffix(".vdf.t8cine-backup")
if not backup.exists():
    shutil.copy2(path, backup)

block = current.group(0)
for key in ("WantsOfflineMode", "SkipOfflineModeWarning"):
    pattern = rf'("{key}"\s*)"\d"'
    if re.search(pattern, block):
        block = re.sub(pattern, rf'\g<1>"{value}"', block)
    else:
        block = block[:-1].rstrip() + f'\n\t\t"{key}"\t\t"{value}"\n\t}}'
path.write_text(text[: current.start()] + block + text[current.end() :], encoding="utf-8")
print("set", command)
