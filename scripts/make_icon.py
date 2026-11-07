import numpy as np, math, struct, io, sys
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 2048                      # supersampled canvas (2x of 1024)
BODY = int(824 * 2)           # Apple icon grid: 824 pt body on 1024 canvas
R = int(186 * 2)              # corner radius
off = (S - BODY) // 2

def lin_grad(w, h, c0, c1, angle_deg):
    a = math.radians(angle_deg)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    t = (x * math.cos(a) + y * math.sin(a))
    t = (t - t.min()) / (t.max() - t.min())
    c0 = np.array(c0, np.float32); c1 = np.array(c1, np.float32)
    img = c0[None, None, :] * (1 - t[..., None]) + c1[None, None, :] * t[..., None]
    return Image.fromarray(img.clip(0, 255).astype(np.uint8), "RGB")

def rounded_mask(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m

canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
body_box = (off, off, off + BODY, off + BODY)
body_mask = rounded_mask((S, S), body_box, R)

# Drop shadow
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
sh_mask = rounded_mask((S, S), (off, off + 24, off + BODY, off + BODY + 24), R).filter(ImageFilter.GaussianBlur(36))
sh.putalpha(sh_mask.point(lambda v: v * 0.55))
canvas = Image.alpha_composite(canvas, sh)

# Body gradient
bg = lin_grad(S, S, (8, 18, 40), (10, 78, 108), 60).convert("RGBA")
# radial vignette / glow in the centre
y, x = np.mgrid[0:S, 0:S].astype(np.float32)
d = np.sqrt((x - S * 0.5) ** 2 + (y - S * 0.56) ** 2) / (S * 0.5)
glow = np.clip(1 - d, 0, 1) ** 2.2
glow_img = np.zeros((S, S, 4), np.float32)
glow_img[..., 0] = 40; glow_img[..., 1] = 170; glow_img[..., 2] = 230
glow_img[..., 3] = glow * 90
bg = Image.alpha_composite(bg, Image.fromarray(glow_img.astype(np.uint8), "RGBA"))

layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
layer.paste(bg, (0, 0), body_mask)
canvas = Image.alpha_composite(canvas, layer)

# Subtle scope grid inside body
grid = Image.new("RGBA", (S, S), (0, 0, 0, 0))
g = ImageDraw.Draw(grid)
step = BODY // 8
for i in range(1, 8):
    xx = off + i * step
    g.line([(xx, off), (xx, off + BODY)], fill=(160, 220, 255, 16), width=3)
    g.line([(off, off + i * step), (off + BODY, off + i * step)], fill=(160, 220, 255, 16), width=3)
grid_m = Image.new("L", (S, S), 0); grid_m.paste(grid.split()[3], (0, 0), body_mask)
grid.putalpha(grid_m)
canvas = Image.alpha_composite(canvas, grid)

def stamp(draw, pts, width, fill):
    # Smooth anti-alias-friendly stroke: dense circles along the path.
    r = width / 2
    dense = []
    for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
        seg = max(1, int(math.hypot(xb - xa, yb - ya) / 1.5))
        for k in range(seg):
            t = k / seg
            dense.append((xa + (xb - xa) * t, ya + (yb - ya) * t))
    dense.append(pts[-1])
    for x, y in dense:
        draw.ellipse((x - r, y - r, x + r, y + r), fill=fill)

def wave_points(freq, amp, phase, yc, x0, x1, n=1600, env=True):
    pts = []
    for i in range(n + 1):
        t = i / n
        x = x0 + (x1 - x0) * t
        e = math.sin(math.pi * t) ** 0.8 if env else 1.0
        y = yc - amp * e * math.sin(2 * math.pi * freq * t + phase)
        pts.append((x, y))
    return pts

x0, x1 = off + BODY * 0.10, off + BODY * 0.90
yc = off + BODY * 0.52

# Input wave (dim, lower frequency)
w_in = Image.new("RGBA", (S, S), (0, 0, 0, 0))
m_in = Image.new("L", (S, S), 0)
stamp(ImageDraw.Draw(m_in), wave_points(2.0, BODY * 0.22, 0.0, yc, x0, x1), 26, 255)
w_in = Image.new("RGBA", (S, S), (150, 175, 200, 0)); w_in.putalpha(m_in.point(lambda v: v * 110 // 255))
canvas = Image.alpha_composite(canvas, w_in)

# Shifted wave (bright gradient stroke + glow)
mask = Image.new("L", (S, S), 0)
stamp(ImageDraw.Draw(mask), wave_points(4.0, BODY * 0.22, 0.0, yc, x0, x1), 40, 255)
stroke_grad = lin_grad(S, S, (70, 230, 255), (255, 90, 210), 0).convert("RGBA")
glow_mask = mask.filter(ImageFilter.GaussianBlur(40)).point(lambda v: min(255, int(v * 1.6)))
gl = stroke_grad.copy(); gl.putalpha(glow_mask)
canvas = Image.alpha_composite(canvas, gl)
st = stroke_grad.copy(); st.putalpha(mask)
canvas = Image.alpha_composite(canvas, st)
# hot core
core = Image.new("RGBA", (S, S), (255, 255, 255, 0))
core_mask = Image.new("L", (S, S), 0)
stamp(ImageDraw.Draw(core_mask), wave_points(4.0, BODY * 0.22, 0.0, yc, x0, x1), 12, 150)
core.putalpha(core_mask)
canvas = Image.alpha_composite(canvas, core)

# Glass highlight on the top half
hl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
hm = Image.new("L", (S, S), 0)
ImageDraw.Draw(hm).ellipse((off - BODY * 0.3, off - BODY * 0.75, off + BODY * 1.3, off + BODY * 0.42), fill=26)
hm = ImageChops.multiply(hm.filter(ImageFilter.GaussianBlur(90)), body_mask)
hl = Image.new("RGBA", (S, S), (255, 255, 255, 0)); hl.putalpha(hm)
canvas = Image.alpha_composite(canvas, hl)

# Thin inner rim
rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(rim).rounded_rectangle(body_box, radius=R, outline=(255, 255, 255, 34), width=5)
canvas = Image.alpha_composite(canvas, rim)

master = canvas.resize((1024, 1024), Image.LANCZOS)
master.save("AppIcon-1024.png")

# Build .icns (PNG-based entries)
entries = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256),
           (b"ic09", 512), (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512)]
chunks = b""
for tag, px in entries:
    buf = io.BytesIO()
    master.resize((px, px), Image.LANCZOS).save(buf, "PNG", optimize=True)
    data = buf.getvalue()
    chunks += tag + struct.pack(">I", len(data) + 8) + data
icns = b"icns" + struct.pack(">I", len(chunks) + 8) + chunks
open("AppIcon.icns", "wb").write(icns)
print("ok", len(icns))
