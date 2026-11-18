"""Draws the Tekken Studio logo (make_svg.py), renders it with headless Chrome, then makes the icon (.ico, several sizes),
the PNGs for GitHub and the README, and the installer's pictures.  Usage: python installer/logo/render.py"""
import pathlib
import runpy
import subprocess
import tempfile

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


def render(svg: pathlib.Path, width: int, height: int, out: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        page = pathlib.Path(tmp) / "page.html"
        page.write_text(f"<html><body style='margin:0;background:transparent'><img src='{svg.as_uri()}' width='{width}' height='{height}'></body></html>")
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--default-background-color=00000000",
                        f"--window-size={width},{height}", f"--screenshot={out}", page.as_uri()], check=True, capture_output=True)


runpy.run_path(str(HERE / "make_svg.py"))
render(HERE / "logo.svg", 1024, 1024, HERE / "logo_1024.png")
icon = Image.open(HERE / "logo_1024.png").convert("RGBA")
icon.resize((512, 512), Image.LANCZOS).save(HERE / "logo_512.png")
icon.save(HERE / "tekken-studio.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
render(HERE / "logo_wide.svg", 2080, 720, HERE / "logo_wide.png")
wide = Image.open(HERE / "logo_wide.png").convert("RGBA")
wide.save(HERE / "logo_wide.png")
# For the README: on dark, since the white TEKKEN would vanish on GitHub's light page.
banner = Image.new("RGBA", (1040, 360), (20, 20, 24, 255))
banner.alpha_composite(wide.resize((1040, 360), Image.LANCZOS))
banner.convert("RGB").save(HERE.parent.parent / "docs" / "images" / "logo.png")
# The installer's small wizard picture: the icon on dark.
small = Image.new("RGB", (138, 140), (22, 22, 26))
small.paste(icon.resize((120, 120), Image.LANCZOS), (9, 10), icon.resize((120, 120), Image.LANCZOS))
small.save(HERE / "wizard_small.bmp")
print("made logo_1024.png logo_512.png tekken-studio.ico logo_wide.png docs/images/logo.png wizard_small.bmp")
# The installer's big picture on its first and last pages (164 x 314): dark, the icon in the middle, a red line below.
large = Image.new("RGB", (164, 314), (16, 16, 20))
mark = icon.resize((140, 140), Image.LANCZOS)
large.paste(mark, (12, 70), mark)
from PIL import ImageDraw
ImageDraw.Draw(large).rectangle((30, 236, 134, 240), fill=(227, 22, 44))
large.save(HERE / "wizard_large.bmp")
print("made wizard_large.bmp")
