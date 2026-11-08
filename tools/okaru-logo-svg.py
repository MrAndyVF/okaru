# Genera assets/okaru-logo.svg (郁 + OKARU) con el arte de bloques del instalador: python3 tools/okaru-logo-svg.py assets/okaru-logo.svg
import sys
KANJI = [
 '      ██       ▄▄▄▄▄▄▄',
 '  ▄██████████▄██▀▀▀███',
 '     ██       ██   ██',
 '   ▄█████████ ██  ██',
 '  ▄███     ██ ██  ██',
 '  ▀▀█████████ ██   ▀█▄',
 '    ██▄▄▄▄▄██ ██    ██',
 '    ██▀▀▀▀▀██ ██  ▄▄██',
 '    ██     ██ ██ ▀▀▀▀',
 '    ██  ▀███▀ ██',
]
WORD = [
 ' ██████╗ ██╗  ██╗ █████╗ ██████╗ ██╗   ██╗',
 '██╔═══██╗██║ ██╔╝██╔══██╗██╔══██╗██║   ██║',
 '██║   ██║█████╔╝ ███████║██████╔╝██║   ██║',
 '██║   ██║██╔═██╗ ██╔══██║██╔══██╗██║   ██║',
 '╚██████╔╝██║  ██╗██║  ██║██║  ██║╚██████╔╝',
 ' ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ',
]
CW, CH = 10, 20           # celda monoespaciada 1:2
T = 1.6                   # grosor de los trazos dobles (sombra)
G = 3.0                   # separación entre las dos líneas de un trazo doble

def cell(ch, x, y):
    r = []
    if ch == '█': r.append((x, y, CW, CH))
    elif ch == '▄': r.append((x, y + CH / 2, CW, CH / 2))
    elif ch == '▀': r.append((x, y, CW, CH / 2))
    else:
        cx, cy = x + CW / 2, y + CH / 2
        h = lambda x0, x1: [(x0, cy - G / 2 - T / 2, x1 - x0, T), (x0, cy + G / 2 - T / 2, x1 - x0, T)]
        v = lambda y0, y1: [(cx - G / 2 - T / 2, y0, T, y1 - y0), (cx + G / 2 - T / 2, y0, T, y1 - y0)]
        if ch == '═': r += h(x, x + CW)
        elif ch == '║': r += v(y, y + CH)
        # esquinas dobles: brazo exterior hasta el borde de la celda, interior hasta el otro trazo
        elif ch == '╗':
            r += [(x, cy - G/2 - T/2, CW/2 + G/2 + T/2, T), (x, cy + G/2 - T/2, CW/2 - G/2 + T/2, T)]
            r += [(cx + G/2 - T/2, cy - G/2 - T/2, T, y + CH - (cy - G/2 - T/2)), (cx - G/2 - T/2, cy + G/2 - T/2, T, y + CH - (cy + G/2 - T/2))]
        elif ch == '╔':
            r += [(cx - G/2 - T/2, cy - G/2 - T/2, x + CW - (cx - G/2 - T/2), T), (cx + G/2 - T/2, cy + G/2 - T/2, x + CW - (cx + G/2 - T/2), T)]
            r += [(cx - G/2 - T/2, cy - G/2 - T/2, T, y + CH - (cy - G/2 - T/2)), (cx + G/2 - T/2, cy + G/2 - T/2, T, y + CH - (cy + G/2 - T/2))]
        elif ch == '╝':
            r += [(x, cy + G/2 - T/2, CW/2 + G/2 + T/2, T), (x, cy - G/2 - T/2, CW/2 - G/2 + T/2, T)]
            r += [(cx + G/2 - T/2, y, T, cy + G/2 + T/2 - y), (cx - G/2 - T/2, y, T, cy - G/2 + T/2 - y)]
        elif ch == '╚':
            r += [(cx - G/2 - T/2, cy + G/2 - T/2, x + CW - (cx - G/2 - T/2), T), (cx + G/2 - T/2, cy - G/2 - T/2, x + CW - (cx + G/2 - T/2), T)]
            r += [(cx - G/2 - T/2, y, T, cy + G/2 + T/2 - y), (cx + G/2 - T/2, y, T, cy - G/2 + T/2 - y)]
    return r

def rects(lines, ox, oy, shadow=None):
    out = []
    for row, line in enumerate(lines):
        for col, ch in enumerate(line):
            if shadow is not None and (ch in '█▄▀') == shadow:
                continue
            out += cell(ch, ox + col * CW, oy + row * CH)
    return out

def path(rs, dx, dy):
    return ''.join(f'M{x - dx:g} {y - dy:g}h{w:g}v{h:g}h{-w:g}z' for (x, y, w, h) in rs)

kw = max(map(len, KANJI)) * CW
gap = 3 * CW
kh, wh = len(KANJI) * CH, len(WORD) * CH
wy = (kh - wh) / 2
kanji = rects(KANJI, 0, 0)
name = rects(WORD, kw + gap, wy, shadow=False)
shadow = rects(WORD, kw + gap, wy, shadow=True)
# El lienzo se recorta al contorno de lo dibujado: el arte tiene columnas vacías desiguales a los lados y,
# sin recortar, el logo no queda centrado.
allr = kanji + name + shadow
x0 = min(x for x, _, _, _ in allr)
y0 = min(y for _, y, _, _ in allr)
W = max(x + w for x, _, w, _ in allr) - x0
H = max(y + h for _, y, _, h in allr) - y0
svg = (f'<?xml version="1.0" encoding="UTF-8"?>\n'
       f'<!-- Logo de OKARU: 郁 + OKARU con el arte de bloques del instalador (tools/okaru-logo-svg.py). '
       f'El kanji usa el color de acento y el nombre el del texto; la shell y el greeter cambian los fill. -->\n'
       f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:g}" height="{H:g}" viewBox="0 0 {W:g} {H:g}">\n'
       f'  <path id="kanji" fill="#7095c8" d="{path(kanji, x0, y0)}"/>\n'
       f'  <path id="name" fill="#ffffff" d="{path(name, x0, y0)}"/>\n'
       f'  <path id="shadow" fill="#ffffff" fill-opacity="0.4" d="{path(shadow, x0, y0)}"/>\n'
       f'</svg>\n')
open(sys.argv[1], 'w').write(svg)
print(f'{W:g} {H:g}')
