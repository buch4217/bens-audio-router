# Erzeugt das App-Icon "Audio + Cloud + Streaming" (Buccloud Software) als ICO + PNG
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import math, pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
S = 1024
def lerp(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
def gradient(size, c1, c2, angle=90):
    img = Image.new("RGB", (size, size)); px = img.load()
    for y in range(size):
        for x in range(size):
            t = (y / (size - 1)) if angle == 90 else (x + y) / (2 * (size - 1))
            px[x, y] = lerp(c1, c2, t)
    return img
# Hintergrund: abgerundetes Quadrat mit Verlauf (tiefes Nachtblau -> Cyan-Blau)
bg = gradient(S, (9, 18, 48), (22, 110, 255), 135).convert("RGBA")
mask = Image.new("L", (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.225), fill=255)
# weicher Lichtfleck oben links
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0)); gd = ImageDraw.Draw(glow)
gd.ellipse((-S * 0.2, -S * 0.3, S * 0.75, S * 0.5), fill=(120, 200, 255, 90)); glow = glow.filter(ImageFilter.GaussianBlur(120))
bg = Image.alpha_composite(bg, glow)
icon = Image.new("RGBA", (S, S), (0, 0, 0, 0)); icon.paste(bg, (0, 0), mask)

# Wolke (weiss mit leichtem Verlauf) + Schatten
def cloud_shape(draw, ox=0, oy=0, fill=255):
    cx, cy = S * 0.50 + ox, S * 0.57 + oy
    draw.rounded_rectangle((cx - S * 0.34, cy - S * 0.06, cx + S * 0.34, cy + S * 0.19), radius=S * 0.125, fill=fill)
    draw.ellipse((cx - S * 0.27, cy - S * 0.24, cx - S * 0.02, cy + S * 0.01 + S * 0.02), fill=fill)
    draw.ellipse((cx - S * 0.10, cy - S * 0.33, cx + S * 0.22, cy - S * 0.01 + S * 0.02), fill=fill)
    draw.ellipse((cx + S * 0.08, cy - S * 0.20, cx + S * 0.31, cy + S * 0.04), fill=fill)
sh = Image.new("L", (S, S), 0); cloud_shape(ImageDraw.Draw(sh), 0, S * 0.022); sh = sh.filter(ImageFilter.GaussianBlur(26))
shadow = Image.new("RGBA", (S, S), (0, 20, 70, 0)); shadow.putalpha(sh.point(lambda v: int(v * 0.55)))
icon = Image.alpha_composite(icon, shadow)
cm = Image.new("L", (S, S), 0); cloud_shape(ImageDraw.Draw(cm))
cloud = gradient(S, (255, 255, 255), (196, 222, 255), 90).convert("RGBA"); cloud.putalpha(cm)
icon = Image.alpha_composite(icon, cloud)

# Equalizer-Balken in der Wolke (Audio)
d = ImageDraw.Draw(icon)
hs = [0.10, 0.20, 0.31, 0.20, 0.12]; bw = S * 0.058; gap = S * 0.036
total = len(hs) * bw + (len(hs) - 1) * gap; x0 = S * 0.50 - total / 2; base = S * 0.69
for i, h in enumerate(hs):
    x = x0 + i * (bw + gap); t = i / (len(hs) - 1)
    col = lerp((0, 190, 255), (30, 90, 255), t) + (255,)
    d.rounded_rectangle((x, base - S * h, x + bw, base), radius=bw / 2, fill=col)

# Streaming-Wellen oben rechts (3 Boegen + Punkt)
cx, cy = S * 0.735, S * 0.295
for i, r in enumerate([S * 0.075, S * 0.135, S * 0.195]):
    a = 255 - i * 45
    d.arc((cx - r, cy - r, cx + r, cy + r), start=-80, end=10, fill=(255, 255, 255, a), width=int(S * 0.026))
d.ellipse((cx - S * 0.022, cy - S * 0.022, cx + S * 0.022, cy + S * 0.022), fill=(255, 255, 255, 255))
# feiner heller Rand
edge = Image.new("RGBA", (S, S), (0, 0, 0, 0)); ImageDraw.Draw(edge).rounded_rectangle((3, 3, S - 4, S - 4), radius=int(S * 0.222), outline=(255, 255, 255, 60), width=6)
icon = Image.alpha_composite(icon, edge)
icon.save(ROOT / "assets/logo_1024.png")
icon.resize((512, 512), Image.LANCZOS).save(ROOT / "assets/logo.png")
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
icon.resize((256, 256), Image.LANCZOS).save(ROOT / "src/app.ico", sizes=[(s, s) for s in sizes])
# Banner (README / Anleitung)
W, H = 1600, 420
ban = gradient(W, (8, 14, 34), (14, 52, 120), 135).crop((0, 0, W, H)).convert("RGBA")
lg = icon.resize((300, 300), Image.LANCZOS); ban.paste(lg, (70, 60), lg)
bd = ImageDraw.Draw(ban)
f1 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 96)
f2 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 38)
f3 = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28)
bd.text((420, 92), "Bens Audio Router", font=f1, fill=(255, 255, 255))
bd.text((424, 214), "Audiophiler Node-Router  ·  Sonos  ·  FLAC 24 Bit  ·  Autopilot", font=f2, fill=(150, 200, 255))
bd.text((424, 300), "Buccloud Software (DE)   ·   www.buccloud.de", font=f3, fill=(200, 215, 240))
ban.convert("RGB").save(ROOT / "assets/banner.png")
print("Icon + Banner erzeugt")
