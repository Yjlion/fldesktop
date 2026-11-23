#!/usr/bin/env python3
"""Generates the stock user pictures into data/faces/ (installed to
/usr/share/pixmaps/faces, where Settings > Users offers them).

Flat motifs on the accent colors of the icon theme, drawn to stay inside the
circle the greeter, the lock screen and Settings clip pictures to. 256 px PNGs;
needs rsvg-convert; deterministic output.
"""
import os
import subprocess
import sys
import tempfile

SIZE = 256
WHITE = "#ffffff"
INK = "#1d2430"


def svg(bg, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 256 256">\n'
            f'  <rect width="256" height="256" fill="{bg}"/>\n{body}</svg>\n')


def sunset():
    return svg("#ed5b00", """
  <circle cx="128" cy="150" r="62" fill="#ffd36b"/>
  <rect x="0" y="150" width="256" height="106" fill="#b8430a"/>
  <rect x="40" y="168" width="176" height="10" rx="5" fill="#ffd36b" opacity="0.55"/>
  <rect x="66" y="190" width="124" height="10" rx="5" fill="#ffd36b" opacity="0.4"/>
  <rect x="94" y="212" width="68" height="10" rx="5" fill="#ffd36b" opacity="0.28"/>
""")


def mountains():
    return svg("#2190a4", """
  <circle cx="178" cy="76" r="22" fill="#e9f6f8"/>
  <path d="M20 206 L100 92 L180 206 Z" fill="#0f5f6e"/>
  <path d="M100 92 L122 124 L108 118 L96 132 L84 120 L78 124 Z" fill="#e9f6f8"/>
  <path d="M96 206 L164 118 L236 206 Z" fill="#14798b"/>
  <path d="M164 118 L182 141 L170 136 L160 146 L150 136 Z" fill="#e9f6f8"/>
""")


def leaf():
    return svg("#3a944a", """
  <path d="M70 192 C 62 120, 110 66, 196 58 C 196 140, 150 194, 70 192 Z" fill="#9fdc8a"/>
  <path d="M70 192 C 110 150, 140 116, 178 78" stroke="#2d7a3a" stroke-width="7" fill="none" stroke-linecap="round"/>
  <path d="M112 146 L 104 112 M136 122 L 134 94 M124 136 L 154 140 M148 110 L 172 112"
        stroke="#2d7a3a" stroke-width="5" fill="none" stroke-linecap="round"/>
""")


def flower():
    petals = "".join(
        f'  <ellipse cx="128" cy="82" rx="24" ry="40" fill="#ffe1ef" transform="rotate({a} 128 128)"/>\n'
        for a in range(0, 360, 60))
    return svg("#d56199", petals + """
  <circle cx="128" cy="128" r="26" fill="#ffc94a"/>
  <circle cx="120" cy="120" r="5" fill="#e8a320"/>
  <circle cx="137" cy="124" r="4" fill="#e8a320"/>
  <circle cx="126" cy="138" r="4" fill="#e8a320"/>
""")


def planet():
    return svg("#9141ac", """
  <circle cx="58" cy="70" r="4" fill="#ffffff" opacity="0.8"/>
  <circle cx="196" cy="58" r="3" fill="#ffffff" opacity="0.7"/>
  <circle cx="204" cy="190" r="4" fill="#ffffff" opacity="0.6"/>
  <circle cx="70" cy="196" r="3" fill="#ffffff" opacity="0.6"/>
  <ellipse cx="128" cy="128" rx="96" ry="26" fill="none" stroke="#e4c6f0" stroke-width="10"
           transform="rotate(-20 128 128)"/>
  <circle cx="128" cy="128" r="54" fill="#f2b8d0"/>
  <path d="M78 116 Q128 100 178 116" stroke="#d98bb0" stroke-width="9" fill="none" stroke-linecap="round"/>
  <path d="M80 144 Q128 160 176 144" stroke="#d98bb0" stroke-width="9" fill="none" stroke-linecap="round"/>
  <path d="M37.8 160.8 A 96 26 -20 0 0 218.2 95.2" fill="none" stroke="#e4c6f0" stroke-width="10"/>
""")


def cat():
    return svg("#6f8396", f"""
  <path d="M66 112 L74 52 L114 90 Z" fill="#f2f2f0"/>
  <path d="M190 112 L182 52 L142 90 Z" fill="#f2f2f0"/>
  <path d="M78 96 L82 70 L100 88 Z" fill="#f4b6c2"/>
  <path d="M178 96 L174 70 L156 88 Z" fill="#f4b6c2"/>
  <ellipse cx="128" cy="140" rx="70" ry="62" fill="#f2f2f0"/>
  <ellipse cx="102" cy="132" rx="9" ry="13" fill="{INK}"/>
  <ellipse cx="154" cy="132" rx="9" ry="13" fill="{INK}"/>
  <path d="M120 156 L136 156 L128 166 Z" fill="#f08aa0"/>
  <path d="M128 166 Q120 178 110 172 M128 166 Q136 178 146 172" stroke="{INK}" stroke-width="4"
        fill="none" stroke-linecap="round"/>
  <path d="M62 152 L92 156 M62 168 L92 164 M194 152 L164 156 M194 168 L164 164" stroke="#9aa6b2"
        stroke-width="3" stroke-linecap="round"/>
""")


def owl():
    return svg("#3584e4", f"""
  <path d="M74 84 L86 54 L106 80 Z M182 84 L170 54 L150 80 Z" fill="#8a5a3c"/>
  <ellipse cx="128" cy="146" rx="66" ry="80" fill="#a06a48"/>
  <ellipse cx="128" cy="172" rx="40" ry="46" fill="#e8cfae"/>
  <path d="M106 160 q6 8 12 0 M128 172 q6 8 12 0 M110 188 q6 8 12 0 M132 196 q6 8 12 0 M118 206 q6 8 12 0"
        stroke="#c4a07c" stroke-width="4" fill="none" stroke-linecap="round"/>
  <circle cx="100" cy="112" r="28" fill="{WHITE}"/>
  <circle cx="156" cy="112" r="28" fill="{WHITE}"/>
  <circle cx="104" cy="114" r="13" fill="{INK}"/>
  <circle cx="152" cy="114" r="13" fill="{INK}"/>
  <circle cx="108" cy="109" r="4" fill="{WHITE}"/>
  <circle cx="156" cy="109" r="4" fill="{WHITE}"/>
  <path d="M120 136 L136 136 L128 152 Z" fill="#f5a623"/>
""")


def fox():
    return svg("#e62d42", f"""
  <path d="M56 70 L96 104 L72 128 Z M200 70 L160 104 L184 128 Z" fill="#ff8a3d"/>
  <path d="M66 84 L86 104 L74 116 Z M190 84 L170 104 L182 116 Z" fill="{INK}"/>
  <path d="M60 110 Q128 64 196 110 L160 176 Q128 214 96 176 Z" fill="#ff8a3d"/>
  <path d="M60 110 Q86 150 128 160 Q170 150 196 110 L168 168 Q128 214 88 168 Z" fill="#fff3e6"/>
  <ellipse cx="102" cy="126" rx="8" ry="10" fill="{INK}"/>
  <ellipse cx="154" cy="126" rx="8" ry="10" fill="{INK}"/>
  <ellipse cx="128" cy="184" rx="12" ry="9" fill="{INK}"/>
""")


def penguin():
    return svg("#1c71d8", f"""
  <ellipse cx="128" cy="144" rx="68" ry="84" fill="{INK}"/>
  <ellipse cx="128" cy="160" rx="48" ry="64" fill="{WHITE}"/>
  <path d="M84 108 Q128 72 172 108 Q160 136 128 128 Q96 136 84 108 Z" fill="{INK}"/>
  <circle cx="108" cy="116" r="9" fill="{WHITE}"/>
  <circle cx="148" cy="116" r="9" fill="{WHITE}"/>
  <circle cx="110" cy="118" r="5" fill="{INK}"/>
  <circle cx="146" cy="118" r="5" fill="{INK}"/>
  <path d="M114 136 L142 136 L128 152 Z" fill="#f5a623"/>
  <ellipse cx="104" cy="226" rx="18" ry="8" fill="#f5a623"/>
  <ellipse cx="152" cy="226" rx="18" ry="8" fill="#f5a623"/>
""")


def coffee():
    return svg("#865e3c", f"""
  <path d="M104 52 q-12 16 0 32 q12 16 0 32 M132 44 q-12 16 0 32 q12 16 0 32"
        stroke="#e8d4bf" stroke-width="7" fill="none" stroke-linecap="round" opacity="0.8"/>
  <path d="M168 136 a26 26 0 1 1 0 52" stroke="{WHITE}" stroke-width="14" fill="none"/>
  <path d="M60 120 L180 120 L170 196 Q166 212 150 212 L90 212 Q74 212 70 196 Z" fill="{WHITE}"/>
  <rect x="60" y="114" width="120" height="14" rx="7" fill="#f1e6da"/>
  <ellipse cx="120" cy="121" rx="54" ry="5" fill="#5a3a22"/>
""")


def rocket():
    return svg("#2b3a55", f"""
  <circle cx="58" cy="62" r="3" fill="{WHITE}" opacity="0.8"/>
  <circle cx="200" cy="80" r="4" fill="{WHITE}" opacity="0.7"/>
  <circle cx="72" cy="186" r="3" fill="{WHITE}" opacity="0.6"/>
  <circle cx="190" cy="196" r="3" fill="{WHITE}" opacity="0.6"/>
  <path d="M110 186 Q128 236 146 186 Z" fill="#ffb02e"/>
  <path d="M118 186 Q128 214 138 186 Z" fill="#ffe28a"/>
  <path d="M100 150 L76 186 L104 178 Z M156 150 L180 186 L152 178 Z" fill="#e62d42"/>
  <path d="M128 40 Q164 76 158 182 L98 182 Q92 76 128 40 Z" fill="#eef1f5"/>
  <path d="M128 40 Q146 58 152 82 L104 82 Q110 58 128 40 Z" fill="#e62d42"/>
  <circle cx="128" cy="116" r="17" fill="#3584e4" stroke="#9aa6b2" stroke-width="6"/>
""")


def waves():
    return svg("#62a0ea", """
  <circle cx="176" cy="78" r="26" fill="#fff4c2"/>
  <path d="M0 138 Q32 116 64 138 T128 138 T192 138 T256 138 L256 256 L0 256 Z" fill="#3584e4"/>
  <path d="M0 172 Q32 150 64 172 T128 172 T192 172 T256 172 L256 256 L0 256 Z" fill="#1c71d8"/>
  <path d="M0 206 Q32 184 64 206 T128 206 T192 206 T256 206 L256 256 L0 256 Z" fill="#1a5fb4"/>
""")


FACES = {
    "sunset": sunset, "mountains": mountains, "leaf": leaf, "flower": flower,
    "planet": planet, "cat": cat, "owl": owl, "fox": fox,
    "penguin": penguin, "coffee": coffee, "rocket": rocket, "waves": waves,
}


def render(svg_text, png):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        name = f.name
    try:
        subprocess.run(["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE), "-o", png, name], check=True)
    finally:
        os.unlink(name)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "data/faces"
    os.makedirs(out, exist_ok=True)
    for name, fn in FACES.items():
        render(fn(), os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main()
