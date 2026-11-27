"""Draws the Station Manager's app icon (app.ico): cyan equalizer bars in an angled red frame, Cyberpunk menu style."""
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
cut = 56
frame = [(10, 10), (S - 10, 10), (S - 10, S - 10 - cut), (S - 10 - cut, S - 10), (10, S - 10)]
d.polygon(frame, fill=(22, 8, 11, 255))
d.line(frame + [frame[0]], fill=(255, 93, 85, 255), width=10, joint="curve")
bars = [0.42, 0.86, 0.62, 0.32]
bw, gap, x0, base = 34, 14, 48, S - 48
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
for i, h in enumerate(bars):
    x = x0 + i * (bw + gap)
    top = base - int(h * 150)
    for dd, im in ((gd, glow), (d, img)):
        dd.rectangle([x, top, x + bw, base], fill=(94, 246, 255, 255))
glow = glow.filter(ImageFilter.GaussianBlur(9))
out = Image.alpha_composite(Image.new("RGBA", (S, S), (0, 0, 0, 0)), img)
out = Image.alpha_composite(glow, out)
out.save(sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
out.save(sys.argv[1].replace(".ico", ".png"))
print("icon written")
