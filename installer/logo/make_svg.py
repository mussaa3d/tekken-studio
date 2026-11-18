"""Draws the Tekken Studio logo as SVG. The mark: a camera's viewfinder (four corner brackets) around a red play button that a
diagonal strike cuts in two. The studio, the replay, the fight. Flat and simple, so it reads at 16 pixels too.
Writes logo.svg (the mark on its tile, for the icon) and logo_wide.svg (the mark and the name, for GitHub and the installer)."""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

RED_TOP, RED_BOTTOM = "#ff3b4a", "#d0142a"

DEFS = f"""
  <defs>
    <linearGradient id="red" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{RED_TOP}"/>
      <stop offset="1" stop-color="{RED_BOTTOM}"/>
    </linearGradient>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#24242c"/>
      <stop offset="1" stop-color="#0e0e12"/>
    </linearGradient>
    <clipPath id="above"><polygon points="0,0 512,0 512,92 0,418"/></clipPath>
    <clipPath id="below"><polygon points="0,440 512,114 512,512 0,512"/></clipPath>
  </defs>"""


def mark(x, y, size):
    """The mark drawn in a 512 box, placed at x, y and scaled to size."""
    k = size / 512
    # Viewfinder: an L at each corner.
    inset, arm, width = 92, 92, 30
    a, b = inset, 512 - inset
    corners = [
        f"M{a},{a + arm} L{a},{a} L{a + arm},{a}",
        f"M{b - arm},{a} L{b},{a} L{b},{a + arm}",
        f"M{b},{b - arm} L{b},{b} L{b - arm},{b}",
        f"M{a + arm},{b} L{a},{b} L{a},{b - arm}",
    ]
    brackets = "".join(f'<path d="{c}" fill="none" stroke="#f2f2f5" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>'
                       for c in corners)
    # The play button, a little to the right of centre so it looks centred, with rounded corners.
    play = "M198,156 Q178,144 178,167 L178,345 Q178,368 198,356 L354,268 Q372,256 354,244 Z"
    # The strike: the triangle in two pieces with a gap between, going up to the right.
    halves = (f'<path d="{play}" fill="url(#red)" clip-path="url(#above)"/>'
              f'<path d="{play}" fill="url(#red)" clip-path="url(#below)" transform="translate(10 8)"/>')
    return f'<g transform="translate({x} {y}) scale({k:.4f})">{brackets}{halves}</g>'


def icon():
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">{DEFS}
  <rect x="8" y="8" width="496" height="496" rx="104" fill="url(#bg)"/>
  {mark(0, 0, 512)}
</svg>
"""


def wide():
    # The name in a heavy slanted face, white with STUDIO in red. The PNGs are rendered on this PC, where Segoe UI Black is there.
    font = "font-family=\"'Segoe UI Black','Arial Black',sans-serif\" font-weight=\"900\""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1040 360" width="1040" height="360">{DEFS}
  <rect x="24" y="44" width="272" height="272" rx="58" fill="url(#bg)"/>
  {mark(24, 44, 272)}
  <g transform="translate(336 0) skewX(-10)">
    <text x="40" y="196" {font} font-size="118" fill="#f2f2f5" letter-spacing="2">TEKKEN</text>
    <text x="40" y="306" {font} font-size="118" fill="url(#red)" letter-spacing="2">STUDIO</text>
  </g>
</svg>
"""


(HERE / "logo.svg").write_text(icon())
(HERE / "logo_wide.svg").write_text(wide())
print("wrote logo.svg and logo_wide.svg")
