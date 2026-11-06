"""Draw our vector-based layered-payload/extraction icon. Optional build tool.

Requires Pillow only to regenerate assets; the checked-in ICO is used by windres.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)
SVG = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
<rect x="8" y="8" width="240" height="240" rx="52" fill="#123746"/>
<path d="M40 155 113 119 186 155 113 191Z" fill="#428d9a"/>
<path d="M40 124 113 88 186 124 113 160Z" fill="#82c8cb"/>
<path d="M40 93 113 57 186 93 113 129Z" fill="#e6f3ef"/>
<path d="M176 119H200V163H220L188 201 156 163H176Z" fill="#f5bb54"/>
</svg>
'''
(ASSETS / "app.svg").write_text(SVG, encoding="utf-8")
im = Image.new("RGBA", (1024, 1024))
d = ImageDraw.Draw(im)
scale = 4
def poly(points, color):
    d.polygon([(x*scale,y*scale) for x,y in points], fill=color)
d.rounded_rectangle((32,32,992,992), radius=208, fill="#123746")
poly([(40,155),(113,119),(186,155),(113,191)],"#428d9a")
poly([(40,124),(113,88),(186,124),(113,160)],"#82c8cb")
poly([(40,93),(113,57),(186,93),(113,129)],"#e6f3ef")
poly([(176,119),(200,119),(200,163),(220,163),(188,201),(156,163),(176,163)],"#f5bb54")
im = im.resize((256,256), Image.Resampling.LANCZOS)
im.save(ASSETS / "app.png")
im.save(ASSETS / "app.ico", sizes=[(n,n) for n in (16,20,24,32,40,48,64,128,256)])
print(ASSETS / "app.ico")
