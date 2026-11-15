"""Draws the tekken8mvm logo as SVG, in the style of the Tekken 8 logo (drawn here, not traced): a slanted red brush-stroke
"8", then MVM in heavy, slanted white letters with brush scratches through them. No fonts, so it renders the same everywhere.
Writes logo_wide.svg (8 + MVM, for Discord, GitHub and the installer) and logo.svg (the 8 alone, for the icon)."""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

DEFS = """
  <defs>
    <linearGradient id="red" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ff3b4a"/>
      <stop offset="0.6" stop-color="#e3162c"/>
      <stop offset="1" stop-color="#a50a1c"/>
    </linearGradient>
    <linearGradient id="white" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#d9dbe2"/>
    </linearGradient>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#22222a"/>
      <stop offset="1" stop-color="#0e0e12"/>
    </linearGradient>
    <!-- brush scratches: long horizontal streaks, cut out of the letters -->
    <filter id="brush" x="-10%" y="-10%" width="120%" height="120%">
      <feTurbulence type="fractalNoise" baseFrequency="0.0018 0.16" numOctaves="1" seed="8" result="noise"/>
      <feColorMatrix in="noise" type="matrix" values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 -16 10.4" result="streaks"/>
      <feComposite in="SourceGraphic" in2="streaks" operator="in" result="scratched"/>
      <feTurbulence type="fractalNoise" baseFrequency="0.04" numOctaves="2" seed="3" result="rough"/>
      <feDisplacementMap in="scratched" in2="rough" scale="7" xChannelSelector="R" yChannelSelector="G"/>
    </filter>
    <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="6"/>
    </filter>
  </defs>"""


def poly(points, dx, dy, k=1.0):
    return "M" + " L".join(f"{dx + x * k:.1f},{dy + y * k:.1f}" for x, y in points) + " Z"


# Heavy, wide letters, 200 high, drawn upright; the group is slanted afterwards. Sharp cuts like the TEKKEN lettering.
M = [(0, 200), (0, 0), (70, 0), (112, 92), (154, 0), (224, 0), (224, 200), (166, 200), (166, 96), (132, 170), (92, 170), (58, 96), (58, 200)]
V = [(0, 0), (62, 0), (106, 128), (150, 0), (212, 0), (138, 200), (74, 200)]


def mvm(dx, dy):
    return " ".join([poly(M, dx, dy), poly(V, dx + 236, dy), poly(M, dx + 456, dy)])


def eight(cx, cy, s=1.0):
    """The 8 as one thick brush stroke: a small top loop and a bigger bottom loop, crossing in the middle."""
    def p(x, y):
        return f"{cx + x * s:.1f},{cy + y * s:.1f}"
    return (f"M{p(4, -6)} C{p(-80, -40)} {p(-78, -150)} {p(4, -150)} C{p(86, -150)} {p(84, -40)} {p(4, -6)} "
            f"C{p(-100, 30)} {p(-104, 150)} {p(4, 150)} C{p(112, 150)} {p(108, 30)} {p(4, -6)} Z")


def eight_layer(cx, cy, s, width):
    path = eight(cx, cy, s)
    return (f'<g transform="skewX(-14)">'
            f'<path d="{path}" fill="none" stroke="#000" stroke-width="{width + 10}" stroke-linejoin="round" opacity="0.55" filter="url(#shadow)"/>'
            f'<path d="{path}" fill="none" stroke="url(#red)" stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round" filter="url(#brush)"/>'
            f'</g>')


def wide():
    letters = mvm(0, 0)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1040 360" width="1040" height="360">{DEFS}
  {eight_layer(230, 180, 1.0, 58)}
  <g transform="translate(330 80) skewX(-16)">
    <path d="{letters}" fill="#000" opacity="0.55" filter="url(#shadow)" transform="translate(6 8)"/>
    <path d="{letters}" fill="url(#white)" filter="url(#brush)"/>
  </g>
</svg>
"""


def icon():
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">{DEFS}
  <rect x="8" y="8" width="496" height="496" rx="104" fill="url(#bg)"/>
  {eight_layer(318, 262, 1.3, 70)}
</svg>
"""


(HERE / "logo_wide.svg").write_text(wide())
(HERE / "logo.svg").write_text(icon())
print("wrote logo_wide.svg and logo.svg")
