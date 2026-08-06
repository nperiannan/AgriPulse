#!/usr/bin/env python3
"""
Generate the Tank Monitor app icon.
Design: dark-blue gradient background, white water tank outline,
        cyan water fill, WiFi monitoring arcs top-right.
Run from the TankMonitor-App project root:
    python scripts/generate_icon.py
"""
import os
from PIL import Image, ImageDraw

SIZE = 1024


def lerp_color(c1, c2, t):
    return tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3)) + (255,)


def gradient_bg(size, c1, c2, corner_radius):
    bg = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(bg)
    for y in range(size):
        draw.line([(0, y), (size - 1, y)], fill=lerp_color(c1, c2, y / (size - 1)))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1],
                                            radius=corner_radius, fill=255)
    result = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    result.paste(bg, mask=mask)
    return result


def run():
    os.makedirs("assets/icon", exist_ok=True)

    # ── Background: dark navy → bright blue gradient ──────────────────────
    img = gradient_bg(SIZE, (10, 40, 110), (25, 118, 210), corner_radius=210)

    # ── Tank geometry ─────────────────────────────────────────────────────
    TX1, TY1, TX2, TY2 = 195, 240, 829, 840
    TR = 68                     # corner radius
    WALL = 26                   # outline thickness

    # Water fill: 72% full, rich cyan-teal
    water_y = TY1 + int((TY2 - TY1) * 0.28)  # 72% from bottom

    water_layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(water_layer).rounded_rectangle(
        [TX1 + WALL // 2 + 1, water_y,
         TX2 - WALL // 2 - 1, TY2 - WALL // 2 - 1],
        radius=TR - 4,
        fill=(41, 182, 246, 220),   # light-blue (#29b6f6)
    )
    img = Image.alpha_composite(img, water_layer)

    draw = ImageDraw.Draw(img)

    # Water surface shimmer (bright white line)
    draw.line(
        [(TX1 + 55, water_y), (TX2 - 55, water_y)],
        fill=(255, 255, 255, 200), width=11,
    )
    # Small ripple arc on the surface
    mid = (TX1 + TX2) // 2
    draw.arc([mid - 110, water_y - 18, mid + 110, water_y + 18],
             start=0, end=180, fill=(255, 255, 255, 120), width=8)

    # Tank white outline
    draw.rounded_rectangle([TX1, TY1, TX2, TY2],
                            radius=TR, outline=(255, 255, 255), width=WALL)

    # ── Inlet pipe on top centre ──────────────────────────────────────────
    PW = 80
    PX = (TX1 + TX2) // 2 - PW // 2
    # Pipe outer (white)
    draw.rectangle([PX, TY1 - 100, PX + PW, TY1 + 6], fill=(255, 255, 255))
    # Pipe inner hollow (matches gradient midpoint colour)
    draw.rectangle([PX + 20, TY1 - 100, PX + PW - 20, TY1 + 6],
                   fill=(25, 118, 210))

    # ── WiFi / signal monitoring arcs (top-right) ─────────────────────────
    SX, SY = 788, 300
    for (r, w, a) in [(52, 19, 255), (90, 15, 210), (130, 12, 160)]:
        draw.arc([SX - r, SY - r, SX + r, SY + r],
                 start=215, end=325,
                 fill=(255, 255, 255, a), width=w)
    # Centre dot of the WiFi symbol
    draw.ellipse([SX - 13, SY + 44, SX + 13, SY + 70],
                 fill=(255, 255, 255))

    # ── Green "OK" status dot (bottom-right of tank) ─────────────────────
    DOT = 52
    draw.ellipse(
        [TX2 - DOT - 20, TY2 - DOT - 20, TX2 - 20, TY2 - 20],
        fill=(82, 196, 26),           # #52c41a – green
        outline=(255, 255, 255), width=8,
    )

    out = "assets/icon/app_icon.png"
    img.save(out)
    print(f"Icon saved → {out}  ({SIZE}×{SIZE})")


if __name__ == "__main__":
    run()
