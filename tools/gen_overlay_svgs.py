#!/usr/bin/env python3
"""
Generate Studio-Grade Story of Seasons SVG Overlay Assets (v1.1.2-hybrid32):
  1. assets/overlay/adaptive.png (1024x1024 RGBA atlas: 8 cols x 8 rows of 128x128 tiles = 32 icons x [idle, held])
     - High-craft Storybook RPG medallions with warm role-coded palettes, bevelled oak/gold rims,
       bold layered vector icons (~70% fill), AND crisp top-right Switch button clue badges
       (Y, A, B, X, L, R, ZL, ZR, -, +, RS, LS) so every button is instantly distinguishable.
     - Includes full Adaptive D-Pad (base + 4 glowing directional petals) AND Adaptive Analog Stick
       (warm oak compass ring base + tactile cream-gold thumb knob) for both Left & Right Sticks!
  2. Refined Pop-Up Quick Menu pill (warm carved wood + golden leaf emblem) patched cleanly into
     assets/overlay/phone.png and assets/overlay/tablet.png.
  3. Generates scratch visual preview (adaptive_hud_preview.png) for quality inspection.
"""
import io
import math
import os
import cairo
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_OV = os.path.join(ROOT, "android_app", "app", "src", "main", "assets", "overlay")
PREVIEW_DIR = "/home/ilhamziqri/.gemini/antigravity/brain/c842777f-6475-43ac-9bfd-caf3c681427f/scratch"

TILE = 128
COLS = 8
ROWS = 8  # 4 icon-rows x 2 states (idle, held) = 32 unique icon tiles!

# ---- Color Palettes (Storybook Warm RPG, Distinct per Button Role) ----
PALETTES = {
    # Golden Sunlit Amber — Main Context Tool (Y)
    "tool": {
        "rim_hi": (1.00, 0.90, 0.56), "rim_lo": (0.76, 0.49, 0.14),
        "bg_hi": (1.00, 0.96, 0.80), "bg_mid": (0.96, 0.78, 0.36), "bg_lo": (0.83, 0.54, 0.17),
        "held_hi": (1.00, 0.98, 0.88), "held_lo": (0.98, 0.72, 0.22),
        "stroke": (0.31, 0.18, 0.07),
    },
    # Meadow Leaf Emerald — Interact / Talk / Confirm / Harvest (A)
    "interact": {
        "rim_hi": (0.84, 0.97, 0.74), "rim_lo": (0.24, 0.54, 0.22),
        "bg_hi": (0.91, 0.99, 0.86), "bg_mid": (0.50, 0.82, 0.42), "bg_lo": (0.26, 0.58, 0.25),
        "held_hi": (0.96, 1.00, 0.92), "held_lo": (0.45, 0.86, 0.38),
        "stroke": (0.14, 0.30, 0.12),
    },
    # Warm Sunset Coral — Dash / Cancel / Back (B)
    "cancel": {
        "rim_hi": (1.00, 0.84, 0.74), "rim_lo": (0.68, 0.25, 0.16),
        "bg_hi": (1.00, 0.92, 0.86), "bg_mid": (0.94, 0.52, 0.38), "bg_lo": (0.74, 0.28, 0.18),
        "held_hi": (1.00, 0.96, 0.90), "held_lo": (0.98, 0.48, 0.32),
        "stroke": (0.36, 0.12, 0.08),
    },
    # Cozy Caramel Leather — Bag / Inventory (X)
    "bag": {
        "rim_hi": (0.98, 0.88, 0.70), "rim_lo": (0.52, 0.31, 0.14),
        "bg_hi": (0.99, 0.94, 0.84), "bg_mid": (0.83, 0.60, 0.36), "bg_lo": (0.58, 0.36, 0.17),
        "held_hi": (1.00, 0.97, 0.88), "held_lo": (0.86, 0.58, 0.28),
        "stroke": (0.28, 0.15, 0.06),
    },
    # Sky Cerulean — Quick Slots (L / R) & Camera (RS / LS)
    "sky": {
        "rim_hi": (0.84, 0.95, 1.00), "rim_lo": (0.22, 0.46, 0.66),
        "bg_hi": (0.92, 0.98, 1.00), "bg_mid": (0.49, 0.76, 0.92), "bg_lo": (0.25, 0.50, 0.70),
        "held_hi": (0.96, 1.00, 1.00), "held_lo": (0.42, 0.76, 0.96),
        "stroke": (0.12, 0.24, 0.38),
    },
    # Doraemon Bell Royal Blue-Gold (ZR)
    "bell": {
        "rim_hi": (1.00, 0.92, 0.54), "rim_lo": (0.16, 0.44, 0.78),
        "bg_hi": (0.86, 0.96, 1.00), "bg_mid": (0.25, 0.66, 0.96), "bg_lo": (0.12, 0.42, 0.76),
        "held_hi": (0.95, 0.99, 1.00), "held_lo": (0.22, 0.66, 0.98),
        "stroke": (0.10, 0.22, 0.42),
    },
    # Warm Parchment Sage — Map / Swap / System / Drawer
    "wood": {
        "rim_hi": (0.96, 0.87, 0.68), "rim_lo": (0.45, 0.29, 0.16),
        "bg_hi": (0.98, 0.95, 0.86), "bg_mid": (0.86, 0.76, 0.58), "bg_lo": (0.64, 0.50, 0.34),
        "held_hi": (1.00, 0.98, 0.90), "held_lo": (0.90, 0.74, 0.46),
        "stroke": (0.27, 0.16, 0.08),
    },
}


def rounded_rect(ctx, x, y, w, h, r):
    r = min(r, w * 0.5, h * 0.5)
    ctx.new_sub_path()
    ctx.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    ctx.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    ctx.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    ctx.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    ctx.close_path()


def draw_medallion(ctx, pal_key, held):
    """Draw a rich multi-layered Story of Seasons medallion occupying ~86% of the 128x128 tile."""
    p = PALETTES[pal_key]
    cx, cy = 64.0, 66.0
    r = 52.0 if not held else 50.0

    # Soft grounded drop shadow
    ctx.save()
    sh = cairo.RadialGradient(cx, cy + 5, r * 0.65, cx, cy + 5, r + 9)
    sh.add_color_stop_rgba(0, 0.14, 0.07, 0.02, 0.48)
    sh.add_color_stop_rgba(1, 0.14, 0.07, 0.02, 0.0)
    ctx.set_source(sh)
    ctx.arc(cx, cy + 5, r + 9, 0, math.tau)
    ctx.fill()
    ctx.restore()

    # Outer dark espresso outline (makes icon pop on bright grass & dark caves alike!)
    ctx.save()
    ctx.arc(cx, cy, r + 2.5, 0, math.tau)
    ctx.set_source_rgba(0.20, 0.11, 0.04, 0.92)
    ctx.fill()
    ctx.restore()

    # Outer metallic/carved bevel rim
    ctx.save()
    rim = cairo.LinearGradient(cx - r, cy - r, cx + r, cy + r)
    rim.add_color_stop_rgba(0.0, *p["rim_hi"], 1.0)
    rim.add_color_stop_rgba(0.55, *p["bg_mid"], 1.0)
    rim.add_color_stop_rgba(1.0, *p["rim_lo"], 1.0)
    ctx.set_source(rim)
    ctx.arc(cx, cy, r, 0, math.tau)
    ctx.fill()
    ctx.restore()

    # Inner recessed face
    ir = r - 6.0
    ctx.save()
    face = cairo.RadialGradient(cx - ir * 0.25, cy - ir * 0.35, 4, cx, cy, ir * 1.1)
    if held:
        face.add_color_stop_rgba(0.0, *p["held_hi"], 1.0)
        face.add_color_stop_rgba(0.65, *p["held_lo"], 1.0)
        face.add_color_stop_rgba(1.0, *p["rim_lo"], 1.0)
    else:
        face.add_color_stop_rgba(0.0, *p["bg_hi"], 0.98)
        face.add_color_stop_rgba(0.60, *p["bg_mid"], 0.96)
        face.add_color_stop_rgba(1.0, *p["bg_lo"], 0.98)
    ctx.set_source(face)
    ctx.arc(cx, cy, ir, 0, math.tau)
    ctx.fill()

    # Top glassy specular crescent
    ctx.save()
    ctx.arc(cx, cy, ir - 1.0, 0, math.tau)
    ctx.clip()
    gloss = cairo.LinearGradient(cx, cy - ir, cx, cy + ir * 0.1)
    gloss.add_color_stop_rgba(0.0, 1.0, 1.0, 1.0, 0.58 if not held else 0.72)
    gloss.add_color_stop_rgba(1.0, 1.0, 1.0, 1.0, 0.0)
    ctx.set_source(gloss)
    ctx.save()
    ctx.translate(cx, cy - ir * 0.42)
    ctx.scale(ir * 0.88, ir * 0.48)
    ctx.arc(0, 0, 1.0, 0, math.tau)
    ctx.restore()
    ctx.fill()
    ctx.restore()

    # Subtle inner hairline border
    ctx.arc(cx, cy, ir, 0, math.tau)
    ctx.set_source_rgba(*p["stroke"], 0.45)
    ctx.set_line_width(2.0)
    ctx.stroke()

    # Held golden halo ring
    if held:
        ctx.arc(cx, cy, r + 3.5, 0, math.tau)
        ctx.set_source_rgba(1.0, 0.95, 0.65, 0.85)
        ctx.set_line_width(3.5)
        ctx.stroke()
    ctx.restore()


def draw_badge(ctx, text):
    """Crisp compact Switch button clue badge at top-right corner of the medallion."""
    if not text:
        return
    ctx.save()
    two = len(text) >= 2
    bw = 38.0 if two else 30.0
    bh = 28.0
    bx = 123.0 - bw
    by = 6.0

    # Badge shadow
    rounded_rect(ctx, bx, by + 2.5, bw, bh, bh * 0.5)
    ctx.set_source_rgba(0.10, 0.05, 0.02, 0.55)
    ctx.fill()

    # Dark espresso wood badge body
    rounded_rect(ctx, bx, by, bw, bh, bh * 0.5)
    bg = cairo.LinearGradient(bx, by, bx, by + bh)
    bg.add_color_stop_rgba(0.0, 0.34, 0.21, 0.13, 0.98)
    bg.add_color_stop_rgba(1.0, 0.18, 0.10, 0.06, 0.98)
    ctx.set_source(bg)
    ctx.fill_preserve()

    # Crisp gold rim
    ctx.set_source_rgba(1.00, 0.89, 0.55, 0.98)
    ctx.set_line_width(2.4)
    ctx.stroke()

    # Crisp bold ivory label
    ctx.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
    ctx.set_font_size(15.5 if two else 18.0)
    ext = ctx.text_extents(text)
    tx = bx + (bw - ext.width) * 0.5 - ext.x_bearing
    ty = by + (bh - ext.height) * 0.5 - ext.y_bearing
    ctx.move_to(tx, ty + 1.0)
    ctx.set_source_rgba(0.0, 0.0, 0.0, 0.6)
    ctx.show_text(text)
    ctx.move_to(tx, ty)
    ctx.set_source_rgba(1.0, 0.98, 0.91, 1.0)
    ctx.show_text(text)
    ctx.restore()


def stroke_fill(ctx, fill_rgba, stroke_rgba=(0.24, 0.13, 0.05, 1.0), lw=3.6):
    ctx.set_source_rgba(*fill_rgba)
    ctx.fill_preserve()
    ctx.set_source_rgba(*stroke_rgba)
    ctx.set_line_width(lw)
    ctx.set_line_join(cairo.LINE_JOIN_ROUND)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke()


def stroke_grad(ctx, grad, stroke_rgba=(0.24, 0.13, 0.05, 1.0), lw=3.6):
    ctx.set_source(grad)
    ctx.fill_preserve()
    ctx.set_source_rgba(*stroke_rgba)
    ctx.set_line_width(lw)
    ctx.set_line_join(cairo.LINE_JOIN_ROUND)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke()


# ======================== HIGH-CRAFT VECTOR ICONS ========================

def ic_hand(ctx):
    """0: Friendly warm open hand + golden action sparkle."""
    ctx.save()
    ctx.translate(62, 68)
    # Palm + 4 fingers + thumb silhouette
    g = cairo.LinearGradient(-20, -28, 22, 28)
    g.add_color_stop_rgba(0, 1.0, 0.96, 0.88, 1.0)
    g.add_color_stop_rgba(1, 0.96, 0.82, 0.66, 1.0)
    # Draw cohesive cartoon glove/hand path
    ctx.move_to(-15, 22)
    ctx.curve_to(-24, 16, -28, 4, -23, -6)
    ctx.curve_to(-20, -11, -13, -8, -12, -1)
    # Index
    ctx.line_to(-12, -23)
    ctx.curve_to(-12, -29, -4, -29, -4, -23)
    ctx.line_to(-4, -8)
    # Middle
    ctx.line_to(-3, -27)
    ctx.curve_to(-3, -33, 5, -33, 5, -27)
    ctx.line_to(5, -8)
    # Ring
    ctx.line_to(7, -23)
    ctx.curve_to(7, -29, 15, -28, 14, -21)
    ctx.line_to(13, -5)
    # Pinky
    ctx.line_to(16, -15)
    ctx.curve_to(17, -20, 23, -18, 22, -12)
    ctx.curve_to(21, 4, 18, 18, 8, 24)
    ctx.curve_to(-2, 27, -10, 26, -15, 22)
    ctx.close_path()
    stroke_grad(ctx, g, (0.28, 0.15, 0.06, 1.0), 3.6)
    # Golden 4-point sparkle
    ctx.translate(-22, -22)
    ctx.move_to(0, -10); ctx.line_to(2.8, -2.8); ctx.line_to(10, 0); ctx.line_to(2.8, 2.8)
    ctx.line_to(0, 10); ctx.line_to(-2.8, 2.8); ctx.line_to(-10, 0); ctx.line_to(-2.8, -2.8)
    ctx.close_path()
    stroke_fill(ctx, (1.0, 0.96, 0.50, 1.0), (0.55, 0.32, 0.04, 1.0), 2.2)
    ctx.restore()


def ic_watering_can(ctx):
    """1: Classic sky-blue farm watering can with brass rose & droplets."""
    ctx.save()
    ctx.translate(64, 68)
    # Handle arc
    ctx.arc(10, -4, 19, -math.pi * 0.65, math.pi * 0.45)
    ctx.set_source_rgba(0.20, 0.48, 0.78, 1.0)
    ctx.set_line_width(7.5)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke_preserve()
    ctx.set_source_rgba(0.12, 0.24, 0.40, 1.0)
    ctx.set_line_width(2.5)
    ctx.stroke()

    # Spout
    ctx.move_to(-8, 8)
    ctx.line_to(-32, -10)
    ctx.line_to(-27, -18)
    ctx.line_to(-3, -4)
    ctx.close_path()
    stroke_fill(ctx, (0.30, 0.68, 0.96, 1.0), (0.12, 0.24, 0.42, 1.0), 3.4)

    # Brass nozzle rose
    ctx.save()
    ctx.translate(-32, -14)
    ctx.rotate(-0.62)
    rounded_rect(ctx, -4, -9, 9, 18, 4)
    stroke_fill(ctx, (1.0, 0.84, 0.34, 1.0), (0.36, 0.22, 0.04, 1.0), 2.8)
    ctx.restore()

    # Can body
    bg = cairo.LinearGradient(-14, -14, 18, 22)
    bg.add_color_stop_rgba(0, 0.56, 0.86, 1.0, 1.0)
    bg.add_color_stop_rgba(0.6, 0.22, 0.58, 0.92, 1.0)
    bg.add_color_stop_rgba(1, 0.14, 0.40, 0.72, 1.0)
    rounded_rect(ctx, -14, -12, 32, 34, 8)
    stroke_grad(ctx, bg, (0.12, 0.24, 0.42, 1.0), 3.6)

    # Top rim band
    rounded_rect(ctx, -15, -15, 34, 8, 4)
    stroke_fill(ctx, (0.98, 0.86, 0.44, 1.0), (0.32, 0.18, 0.05, 1.0), 2.6)

    # Sparkling water droplets
    for dx, dy, sc in [(-37, 2, 1.0), (-30, 14, 0.85), (-21, 20, 0.78)]:
        ctx.save()
        ctx.translate(dx, dy)
        ctx.scale(sc, sc)
        ctx.move_to(0, -6)
        ctx.curve_to(5, 0, 5, 6, 0, 6)
        ctx.curve_to(-5, 6, -5, 0, 0, -6)
        ctx.close_path()
        stroke_fill(ctx, (0.76, 0.96, 1.0, 1.0), (0.14, 0.42, 0.72, 1.0), 2.2)
        ctx.restore()
    ctx.restore()


def ic_hoe(ctx):
    """2: Sturdy wooden-handle farm hoe with gleaming forged steel blade."""
    ctx.save()
    ctx.translate(64, 68)
    # Diagonal oak handle
    ctx.save()
    ctx.rotate(-0.72)
    hg = cairo.LinearGradient(-6, -34, 6, 34)
    hg.add_color_stop_rgba(0, 0.82, 0.56, 0.30, 1.0)
    hg.add_color_stop_rgba(1, 0.50, 0.28, 0.12, 1.0)
    rounded_rect(ctx, -5, -34, 10, 70, 5)
    stroke_grad(ctx, hg, (0.24, 0.12, 0.04, 1.0), 3.4)
    ctx.restore()

    # Forged steel hoe blade (top-left sweeping down-left)
    bg = cairo.LinearGradient(-34, -28, 4, 10)
    bg.add_color_stop_rgba(0, 0.94, 0.97, 1.0, 1.0)
    bg.add_color_stop_rgba(0.55, 0.68, 0.76, 0.84, 1.0)
    bg.add_color_stop_rgba(1, 0.42, 0.48, 0.56, 1.0)
    ctx.move_to(-26, -24)
    ctx.line_to(10, -28)
    ctx.line_to(14, -17)
    ctx.line_to(-12, -12)
    ctx.line_to(-22, 14)
    ctx.line_to(-34, 6)
    ctx.close_path()
    stroke_grad(ctx, bg, (0.18, 0.20, 0.25, 1.0), 3.6)

    # Brass collar rivet
    ctx.arc(-14, -18, 5.5, 0, math.tau)
    stroke_fill(ctx, (1.0, 0.84, 0.36, 1.0), (0.32, 0.18, 0.04, 1.0), 2.5)
    ctx.restore()


def ic_hammer(ctx):
    """3: Heavy mining hammer / mallet with iron head & bright glint."""
    ctx.save()
    ctx.translate(64, 68)
    ctx.rotate(-0.65)
    # Handle
    hg = cairo.LinearGradient(-5, -10, 5, 36)
    hg.add_color_stop_rgba(0, 0.84, 0.56, 0.28, 1.0)
    hg.add_color_stop_rgba(1, 0.50, 0.27, 0.11, 1.0)
    rounded_rect(ctx, -5.5, -12, 11, 48, 5)
    stroke_grad(ctx, hg, (0.24, 0.12, 0.04, 1.0), 3.4)
    # Iron head
    mg = cairo.LinearGradient(-26, -30, 26, -4)
    mg.add_color_stop_rgba(0, 0.92, 0.95, 0.98, 1.0)
    mg.add_color_stop_rgba(0.5, 0.58, 0.64, 0.72, 1.0)
    mg.add_color_stop_rgba(1, 0.34, 0.38, 0.46, 1.0)
    rounded_rect(ctx, -26, -30, 52, 24, 6)
    stroke_grad(ctx, mg, (0.16, 0.18, 0.24, 1.0), 3.6)
    # Gold center band
    rounded_rect(ctx, -8, -31, 16, 26, 3)
    stroke_fill(ctx, (0.98, 0.80, 0.32, 1.0), (0.34, 0.20, 0.04, 1.0), 2.6)
    ctx.restore()


def ic_axe(ctx):
    """4: Woodcutter axe with curved bright steel edge."""
    ctx.save()
    ctx.translate(64, 68)
    ctx.rotate(-0.65)
    # Oak handle
    hg = cairo.LinearGradient(-5, -34, 5, 36)
    hg.add_color_stop_rgba(0, 0.84, 0.56, 0.28, 1.0)
    hg.add_color_stop_rgba(1, 0.48, 0.26, 0.10, 1.0)
    rounded_rect(ctx, -5.5, -32, 11, 68, 5)
    stroke_grad(ctx, hg, (0.24, 0.12, 0.04, 1.0), 3.4)
    # Curved axe blade
    bg = cairo.LinearGradient(-28, -28, 14, 0)
    bg.add_color_stop_rgba(0, 0.97, 0.99, 1.0, 1.0)
    bg.add_color_stop_rgba(0.6, 0.66, 0.74, 0.82, 1.0)
    bg.add_color_stop_rgba(1, 0.40, 0.46, 0.54, 1.0)
    ctx.move_to(12, -26)
    ctx.line_to(-14, -30)
    ctx.curve_to(-32, -20, -32, -2, -14, 6)
    ctx.line_to(12, -4)
    ctx.close_path()
    stroke_grad(ctx, bg, (0.16, 0.18, 0.24, 1.0), 3.6)
    # Bright cutting edge highlight
    ctx.move_to(-16, -25)
    ctx.curve_to(-26, -17, -26, -5, -16, 1)
    ctx.set_source_rgba(1.0, 1.0, 1.0, 0.85)
    ctx.set_line_width(3.0)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke()
    ctx.restore()


def ic_sickle(ctx):
    """5: Crescent harvest sickle with wooden grip."""
    ctx.save()
    ctx.translate(64, 68)
    # Crescent blade
    bg = cairo.LinearGradient(-28, -32, 26, 10)
    bg.add_color_stop_rgba(0, 0.96, 0.98, 1.0, 1.0)
    bg.add_color_stop_rgba(0.6, 0.68, 0.76, 0.84, 1.0)
    bg.add_color_stop_rgba(1, 0.42, 0.48, 0.56, 1.0)
    ctx.move_to(-26, -4)
    ctx.curve_to(-26, -34, 12, -38, 28, -14)
    ctx.curve_to(32, -4, 24, 6, 14, 10)
    ctx.line_to(6, 2)
    ctx.curve_to(16, -4, 16, -20, -2, -20)
    ctx.curve_to(-14, -20, -20, -12, -26, -4)
    ctx.close_path()
    stroke_grad(ctx, bg, (0.16, 0.18, 0.24, 1.0), 3.5)
    # Wooden handle
    ctx.save()
    ctx.translate(4, 18)
    ctx.rotate(0.55)
    rounded_rect(ctx, -6, -14, 12, 30, 5)
    stroke_fill(ctx, (0.76, 0.48, 0.22, 1.0), (0.24, 0.12, 0.04, 1.0), 3.4)
    ctx.restore()
    ctx.restore()


def ic_fishing_rod(ctx):
    """6: Bamboo fishing rod + arched line + red/white bobber."""
    ctx.save()
    ctx.translate(64, 68)
    # Bamboo rod diagonal
    ctx.move_to(-28, 26)
    ctx.curve_to(-10, 6, 4, -14, 22, -28)
    ctx.set_source_rgba(0.24, 0.12, 0.04, 1.0)
    ctx.set_line_width(10.5)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke_preserve()
    ctx.set_source_rgba(0.84, 0.60, 0.26, 1.0)
    ctx.set_line_width(5.5)
    ctx.stroke()

    # Fishing line down to bobber
    ctx.move_to(22, -28)
    ctx.curve_to(28, -12, 20, -2, 18, 6)
    ctx.set_source_rgba(0.95, 0.99, 1.0, 0.95)
    ctx.set_line_width(2.5)
    ctx.stroke()

    # Red & white bobber float
    ctx.save()
    ctx.translate(18, 16)
    ctx.arc(0, 0, 12, math.pi, math.tau)
    stroke_fill(ctx, (0.94, 0.26, 0.20, 1.0), (0.28, 0.08, 0.06, 1.0), 3.0)
    ctx.arc(0, 0, 12, 0, math.pi)
    stroke_fill(ctx, (1.0, 0.99, 0.96, 1.0), (0.28, 0.16, 0.08, 1.0), 3.0)
    ctx.restore()

    # Water ripple underneath
    ctx.move_to(2, 30); ctx.curve_to(10, 26, 26, 26, 34, 30)
    ctx.set_source_rgba(0.32, 0.78, 1.0, 0.95)
    ctx.set_line_width(3.4)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke()
    ctx.restore()


def ic_bug_net(ctx):
    """7: Bug catching net with white woven mesh & golden butterfly."""
    ctx.save()
    ctx.translate(64, 68)
    # Handle
    ctx.move_to(-26, 28)
    ctx.line_to(4, -2)
    ctx.set_source_rgba(0.24, 0.12, 0.04, 1.0)
    ctx.set_line_width(10.5)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke_preserve()
    ctx.set_source_rgba(0.84, 0.56, 0.28, 1.0)
    ctx.set_line_width(5.5)
    ctx.stroke()

    # Net bag
    ctx.move_to(0, -14)
    ctx.curve_to(14, -34, 34, -20, 28, -2)
    ctx.curve_to(22, 16, 4, 24, -4, 12)
    ctx.close_path()
    stroke_fill(ctx, (0.95, 0.98, 1.0, 0.92), (0.24, 0.36, 0.48, 1.0), 3.2)

    # Hoop ring
    ctx.save()
    ctx.translate(6, -8)
    ctx.rotate(-0.7)
    ctx.scale(1.0, 0.65)
    ctx.arc(0, 0, 18, 0, math.tau)
    ctx.restore()
    ctx.set_source_rgba(0.55, 0.30, 0.12, 1.0)
    ctx.set_line_width(4.5)
    ctx.stroke()

    # Tiny cute butterfly top-left
    ctx.save()
    ctx.translate(-22, -16)
    ctx.rotate(-0.2)
    ctx.arc(-5, -3, 6.5, 0, math.tau)
    ctx.arc(5, -3, 6.5, 0, math.tau)
    stroke_fill(ctx, (1.0, 0.88, 0.32, 1.0), (0.40, 0.22, 0.04, 1.0), 2.2)
    ctx.restore()
    ctx.restore()


def ic_seeds(ctx):
    """8: Tied burlap seed pouch with bright green sprout emblem."""
    ctx.save()
    ctx.translate(64, 68)
    # Pouch top flare
    ctx.move_to(-12, -26)
    ctx.line_to(12, -26)
    ctx.line_to(8, -14)
    ctx.line_to(-8, -14)
    ctx.close_path()
    stroke_fill(ctx, (0.90, 0.78, 0.56, 1.0), (0.30, 0.18, 0.07, 1.0), 3.4)

    # Plump seed sack body
    pg = cairo.RadialGradient(-6, -2, 4, 0, 6, 30)
    pg.add_color_stop_rgba(0, 0.98, 0.91, 0.74, 1.0)
    pg.add_color_stop_rgba(1, 0.78, 0.60, 0.36, 1.0)
    ctx.move_to(-10, -14)
    ctx.curve_to(-28, -4, -30, 22, -14, 28)
    ctx.line_to(14, 28)
    ctx.curve_to(30, 22, 28, -4, 10, -14)
    ctx.close_path()
    stroke_grad(ctx, pg, (0.30, 0.18, 0.07, 1.0), 3.6)

    # Red tie ribbon
    rounded_rect(ctx, -12, -17, 24, 6, 3)
    stroke_fill(ctx, (0.88, 0.28, 0.20, 1.0), (0.32, 0.08, 0.06, 1.0), 2.4)

    # Fresh green sprout on front
    ctx.move_to(0, 18); ctx.line_to(0, 4)
    ctx.set_source_rgba(0.18, 0.52, 0.16, 1.0)
    ctx.set_line_width(3.6)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.stroke()
    # Two leaves
    for sgn in (-1, 1):
        ctx.move_to(0, 8)
        ctx.curve_to(sgn * 6, -4, sgn * 16, -2, sgn * 14, 8)
        ctx.curve_to(sgn * 10, 14, sgn * 3, 12, 0, 8)
        ctx.close_path()
        stroke_fill(ctx, (0.44, 0.84, 0.30, 1.0), (0.14, 0.38, 0.12, 1.0), 2.6)
    ctx.restore()


def ic_animal_care(ctx):
    """9: Animal care heart + grooming sparkle (Brush / Milker / Shears / Bell / Feed)."""
    ctx.save()
    ctx.translate(64, 70)
    hg = cairo.LinearGradient(0, -28, 0, 28)
    hg.add_color_stop_rgba(0, 1.0, 0.62, 0.72, 1.0)
    hg.add_color_stop_rgba(1, 0.88, 0.24, 0.38, 1.0)
    ctx.move_to(0, 24)
    ctx.curve_to(-32, 4, -34, -22, -15, -26)
    ctx.curve_to(-5, -28, 0, -18, 0, -14)
    ctx.curve_to(0, -18, 5, -28, 15, -26)
    ctx.curve_to(34, -22, 32, 4, 0, 24)
    ctx.close_path()
    stroke_grad(ctx, hg, (0.38, 0.08, 0.14, 1.0), 3.6)
    # Gloss dot
    ctx.arc(-12, -13, 5.0, 0, math.tau)
    ctx.set_source_rgba(1.0, 0.92, 0.94, 0.85)
    ctx.fill()
    ctx.restore()


def ic_carry_gift(ctx):
    """10: Carrying crop/apple/gift item (Rucksack item / Carrying state)."""
    ctx.save()
    ctx.translate(64, 70)
    # Plump shiny turnip/apple harvest item
    ag = cairo.RadialGradient(-8, -6, 4, 0, 4, 28)
    ag.add_color_stop_rgba(0, 1.0, 0.56, 0.42, 1.0)
    ag.add_color_stop_rgba(1, 0.82, 0.18, 0.16, 1.0)
    ctx.move_to(0, -14)
    ctx.curve_to(-24, -22, -32, 8, -14, 24)
    ctx.curve_to(-6, 28, 0, 24, 0, 22)
    ctx.curve_to(0, 24, 6, 28, 14, 24)
    ctx.curve_to(32, 8, 24, -22, 0, -14)
    ctx.close_path()
    stroke_grad(ctx, ag, (0.36, 0.08, 0.06, 1.0), 3.6)
    # Leaf
    ctx.move_to(2, -15)
    ctx.curve_to(8, -32, 24, -30, 20, -18)
    ctx.curve_to(14, -12, 6, -12, 2, -15)
    ctx.close_path()
    stroke_fill(ctx, (0.46, 0.86, 0.30, 1.0), (0.14, 0.38, 0.10, 1.0), 2.8)
    # Specular highlight
    ctx.arc(-11, -2, 5.0, 0, math.tau)
    ctx.set_source_rgba(1.0, 0.94, 0.88, 0.80)
    ctx.fill()
    ctx.restore()


def ic_talk(ctx):
    """11: Cozy cream speech bubble with warm dots (Interact / Talk - A)."""
    ctx.save()
    ctx.translate(64, 66)
    bg = cairo.LinearGradient(0, -24, 0, 26)
    bg.add_color_stop_rgba(0, 1.0, 0.99, 0.94, 1.0)
    bg.add_color_stop_rgba(1, 0.94, 0.90, 0.78, 1.0)
    ctx.move_to(-28, -6)
    ctx.curve_to(-28, -24, -14, -28, 0, -28)
    ctx.curve_to(16, -28, 30, -22, 30, -4)
    ctx.curve_to(30, 12, 16, 18, 2, 18)
    ctx.line_to(-16, 28)
    ctx.line_to(-12, 14)
    ctx.curve_to(-24, 10, -28, 4, -28, -6)
    ctx.close_path()
    stroke_grad(ctx, bg, (0.14, 0.30, 0.12, 1.0), 3.8)
    # Three warm conversation dots
    for dx in (-12, 0, 12):
        ctx.arc(dx, -5, 4.4, 0, math.tau)
        ctx.set_source_rgba(0.24, 0.56, 0.24, 1.0)
        ctx.fill()
    ctx.restore()


def ic_confirm(ctx):
    """12: Bold storybook checkmark (Menu / Title Confirm - A)."""
    ctx.save()
    ctx.translate(64, 68)
    ctx.move_to(-24, 2)
    ctx.line_to(-7, 20)
    ctx.line_to(26, -18)
    ctx.set_line_join(cairo.LINE_JOIN_ROUND)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.set_source_rgba(0.14, 0.30, 0.12, 1.0)
    ctx.set_line_width(16.0)
    ctx.stroke_preserve()
    ctx.set_source_rgba(1.0, 0.99, 0.92, 1.0)
    ctx.set_line_width(9.5)
    ctx.stroke()
    ctx.restore()


def ic_harvest_give(ctx):
    """13: Harvest / Give hand with heart (Carrying item - A)."""
    ctx.save()
    ctx.translate(64, 68)
    # Small heart above open cradling palm
    ctx.move_to(0, -2)
    ctx.curve_to(-16, -12, -16, -26, -7, -28)
    ctx.curve_to(-2, -29, 0, -23, 0, -21)
    ctx.curve_to(0, -23, 2, -29, 7, -28)
    ctx.curve_to(16, -26, 16, -12, 0, -2)
    ctx.close_path()
    stroke_fill(ctx, (1.0, 0.42, 0.50, 1.0), (0.36, 0.08, 0.12, 1.0), 3.0)
    # Cradling hand below
    ctx.move_to(-28, 12)
    ctx.line_to(-10, 4)
    ctx.line_to(10, 8)
    ctx.curve_to(24, 6, 30, 14, 20, 20)
    ctx.line_to(-6, 26)
    ctx.line_to(-28, 22)
    ctx.close_path()
    stroke_fill(ctx, (1.0, 0.96, 0.88, 1.0), (0.20, 0.32, 0.14, 1.0), 3.4)
    ctx.restore()


def ic_dash_cancel(ctx):
    """14: Bold dynamic curved return / dash arrow (B)."""
    ctx.save()
    ctx.translate(64, 68)
    ctx.move_to(-28, -4)
    ctx.line_to(-4, -26)
    ctx.line_to(-4, -12)
    ctx.curve_to(18, -12, 28, 0, 24, 22)
    ctx.curve_to(14, 8, 6, 6, -4, 6)
    ctx.line_to(-4, 18)
    ctx.close_path()
    stroke_fill(ctx, (1.0, 0.97, 0.90, 1.0), (0.38, 0.12, 0.08, 1.0), 3.8)
    ctx.restore()


def ic_bag(ctx):
    """15: Farmer stitched leather satchel with brass buckle (X)."""
    ctx.save()
    ctx.translate(64, 70)
    # Top handle
    ctx.arc(0, -18, 12, math.pi, math.tau)
    ctx.set_source_rgba(0.28, 0.14, 0.05, 1.0)
    ctx.set_line_width(7.5)
    ctx.stroke_preserve()
    ctx.set_source_rgba(0.92, 0.72, 0.42, 1.0)
    ctx.set_line_width(3.5)
    ctx.stroke()
    # Bag body
    rounded_rect(ctx, -26, -14, 52, 38, 9)
    stroke_fill(ctx, (0.98, 0.86, 0.64, 1.0), (0.28, 0.14, 0.05, 1.0), 3.6)
    # Flap
    ctx.move_to(-26, -10)
    ctx.line_to(26, -10)
    ctx.line_to(26, 2)
    ctx.curve_to(10, 10, -10, 10, -26, 2)
    ctx.close_path()
    stroke_fill(ctx, (0.86, 0.56, 0.28, 1.0), (0.28, 0.14, 0.05, 1.0), 3.4)
    # Brass clasp
    rounded_rect(ctx, -7, -2, 14, 14, 4)
    stroke_fill(ctx, (1.0, 0.88, 0.36, 1.0), (0.34, 0.18, 0.04, 1.0), 2.8)
    ctx.restore()


def ic_chevron(ctx, direction):
    """16/17: Bold crisp double chevron for L / R quick-slot switch."""
    ctx.save()
    ctx.translate(64, 68)
    if direction > 0:
        ctx.scale(-1, 1)
    for ox in (-8, 12):
        ctx.move_to(ox + 10, -20)
        ctx.line_to(ox - 10, 0)
        ctx.line_to(ox + 10, 20)
        ctx.set_line_join(cairo.LINE_JOIN_ROUND)
        ctx.set_line_cap(cairo.LINE_CAP_ROUND)
        ctx.set_source_rgba(0.12, 0.24, 0.38, 1.0)
        ctx.set_line_width(14.0)
        ctx.stroke_preserve()
        ctx.set_source_rgba(1.0, 0.99, 0.95, 1.0)
        ctx.set_line_width(8.0)
        ctx.stroke()
    ctx.restore()


def ic_swap(ctx):
    """18: Pocket Swap dual arrows (ZL)."""
    ctx.save()
    ctx.translate(64, 68)
    for rot in (0, math.pi):
        ctx.save()
        ctx.rotate(rot)
        ctx.arc(0, 0, 20, -math.pi * 0.82, -math.pi * 0.12)
        ctx.set_source_rgba(0.24, 0.14, 0.05, 1.0)
        ctx.set_line_width(11.0)
        ctx.set_line_cap(cairo.LINE_CAP_ROUND)
        ctx.stroke_preserve()
        ctx.set_source_rgba(1.0, 0.98, 0.90, 1.0)
        ctx.set_line_width(5.8)
        ctx.stroke()
        # Arrowhead
        ctx.move_to(24, -4); ctx.line_to(12, -16); ctx.line_to(26, -18); ctx.close_path()
        stroke_fill(ctx, (1.0, 0.98, 0.90, 1.0), (0.24, 0.14, 0.05, 1.0), 2.8)
        ctx.restore()
    ctx.restore()


def ic_bell(ctx):
    """19: Iconic golden Doraemon Bell with red collar ribbon (ZR)."""
    ctx.save()
    ctx.translate(64, 70)
    # Red collar band
    rounded_rect(ctx, -26, -26, 52, 11, 5)
    stroke_fill(ctx, (0.92, 0.20, 0.18, 1.0), (0.34, 0.06, 0.06, 1.0), 3.0)
    # Golden sphere
    bg = cairo.RadialGradient(-7, -4, 4, 0, 4, 26)
    bg.add_color_stop_rgba(0, 1.0, 0.96, 0.62, 1.0)
    bg.add_color_stop_rgba(0.6, 0.98, 0.80, 0.18, 1.0)
    bg.add_color_stop_rgba(1, 0.80, 0.56, 0.06, 1.0)
    ctx.arc(0, 4, 23, 0, math.tau)
    stroke_grad(ctx, bg, (0.34, 0.20, 0.04, 1.0), 3.6)
    # Horizontal bell ridge
    rounded_rect(ctx, -24, -5, 48, 7, 3)
    stroke_fill(ctx, (1.0, 0.90, 0.42, 1.0), (0.34, 0.20, 0.04, 1.0), 2.6)
    # Keyhole dot & slit
    ctx.arc(0, 11, 5.2, 0, math.tau)
    ctx.set_source_rgba(0.26, 0.15, 0.04, 1.0)
    ctx.fill()
    ctx.move_to(0, 14); ctx.line_to(0, 26)
    ctx.set_line_width(4.2)
    ctx.stroke()
    ctx.restore()


def ic_map(ctx):
    """20: Folded world map parchment with red location pin (-)."""
    ctx.save()
    ctx.translate(64, 70)
    # 3-panel folded map
    ctx.move_to(-28, -14); ctx.line_to(-10, -20); ctx.line_to(10, -14); ctx.line_to(28, -20)
    ctx.line_to(28, 18); ctx.line_to(10, 24); ctx.line_to(-10, 18); ctx.line_to(-28, 24)
    ctx.close_path()
    stroke_fill(ctx, (0.99, 0.95, 0.82, 1.0), (0.28, 0.16, 0.07, 1.0), 3.4)
    # Fold creases
    ctx.move_to(-10, -20); ctx.line_to(-10, 18)
    ctx.move_to(10, -14); ctx.line_to(10, 24)
    ctx.set_source_rgba(0.55, 0.42, 0.24, 0.7)
    ctx.set_line_width(2.2)
    ctx.stroke()
    # Red pin
    ctx.move_to(0, 8)
    ctx.curve_to(-12, -4, -12, -18, 0, -18)
    ctx.curve_to(12, -18, 12, -4, 0, 8)
    ctx.close_path()
    stroke_fill(ctx, (0.94, 0.24, 0.20, 1.0), (0.34, 0.06, 0.06, 1.0), 2.6)
    ctx.arc(0, -10, 3.5, 0, math.tau)
    ctx.set_source_rgba(1.0, 0.98, 0.94, 1.0)
    ctx.fill()
    ctx.restore()


def ic_journal(ctx):
    """21: Open storybook / journal (+)."""
    ctx.save()
    ctx.translate(64, 69)
    # Left page
    ctx.move_to(0, -16); ctx.curve_to(-12, -22, -24, -20, -28, -14)
    ctx.line_to(-28, 20); ctx.curve_to(-18, 14, -8, 16, 0, 22); ctx.close_path()
    stroke_fill(ctx, (1.0, 0.98, 0.90, 1.0), (0.28, 0.16, 0.06, 1.0), 3.4)
    # Right page
    ctx.move_to(0, -16); ctx.curve_to(12, -22, 24, -20, 28, -14)
    ctx.line_to(28, 20); ctx.curve_to(18, 14, 8, 16, 0, 22); ctx.close_path()
    stroke_fill(ctx, (0.98, 0.94, 0.80, 1.0), (0.28, 0.16, 0.06, 1.0), 3.4)
    # Page lines
    ctx.set_source_rgba(0.50, 0.36, 0.20, 0.75)
    ctx.set_line_width(2.6)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    for ly in (-6, 2, 10):
        ctx.move_to(-21, ly); ctx.line_to(-6, ly)
        ctx.move_to(6, ly); ctx.line_to(21, ly)
    ctx.stroke()
    ctx.restore()


def ic_camera(ctx):
    """22: Camera lens + reset icon (RS)."""
    ctx.save()
    ctx.translate(64, 70)
    rounded_rect(ctx, -26, -14, 52, 34, 8)
    stroke_fill(ctx, (0.98, 0.97, 0.93, 1.0), (0.14, 0.26, 0.40, 1.0), 3.4)
    # Top viewfinder bump
    rounded_rect(ctx, -12, -23, 24, 11, 4)
    stroke_fill(ctx, (0.98, 0.97, 0.93, 1.0), (0.14, 0.26, 0.40, 1.0), 3.2)
    # Lens
    ctx.arc(0, 3, 12, 0, math.tau)
    stroke_fill(ctx, (0.34, 0.70, 0.96, 1.0), (0.14, 0.26, 0.40, 1.0), 3.2)
    ctx.arc(-3, 0, 4.0, 0, math.tau)
    ctx.set_source_rgba(1.0, 1.0, 1.0, 0.85)
    ctx.fill()
    ctx.restore()


def ic_sprint_ls(ctx):
    """23: Compass star / quick focus (LS)."""
    ctx.save()
    ctx.translate(64, 68)
    ctx.move_to(0, -26); ctx.line_to(8, -8); ctx.line_to(26, 0); ctx.line_to(8, 8)
    ctx.line_to(0, 26); ctx.line_to(-8, 8); ctx.line_to(-26, 0); ctx.line_to(-8, -8)
    ctx.close_path()
    stroke_fill(ctx, (1.0, 0.98, 0.88, 1.0), (0.14, 0.26, 0.40, 1.0), 3.4)
    ctx.arc(0, 0, 6.5, 0, math.tau)
    stroke_fill(ctx, (1.0, 0.84, 0.30, 1.0), (0.28, 0.16, 0.04, 1.0), 2.4)
    ctx.restore()


def ic_drawer_open(ctx):
    """24: Three golden clover dots (expand drawer)."""
    ctx.save()
    ctx.translate(64, 66)
    for dx in (-18, 0, 18):
        ctx.arc(dx, 0, 7.2, 0, math.tau)
        stroke_fill(ctx, (1.0, 0.97, 0.86, 1.0), (0.28, 0.16, 0.06, 1.0), 3.0)
    ctx.restore()


def ic_drawer_close(ctx):
    """25: Crisp X close icon (collapse drawer)."""
    ctx.save()
    ctx.translate(64, 66)
    ctx.move_to(-16, -16); ctx.line_to(16, 16)
    ctx.move_to(16, -16); ctx.line_to(-16, 16)
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    ctx.set_source_rgba(0.28, 0.16, 0.06, 1.0)
    ctx.set_line_width(14.0)
    ctx.stroke_preserve()
    ctx.set_source_rgba(1.0, 0.97, 0.86, 1.0)
    ctx.set_line_width(8.0)
    ctx.stroke()
    ctx.restore()


# ======================== ADAPTIVE D-PAD & ANALOG STICKS ========================

def draw_dpad_base(ctx, held):
    """26: Carved warm oak Story of Seasons cross D-Pad with golden directional arrows."""
    ctx.save()
    cx, cy = 64.0, 64.0
    # Subtle circular wood plate behind cross
    ctx.arc(cx, cy, 56, 0, math.tau)
    bg = cairo.RadialGradient(cx, cy - 10, 6, cx, cy, 56)
    bg.add_color_stop_rgba(0, 0.38, 0.23, 0.13, 0.62)
    bg.add_color_stop_rgba(1, 0.20, 0.11, 0.06, 0.72)
    ctx.set_source(bg)
    ctx.fill_preserve()
    ctx.set_source_rgba(0.96, 0.84, 0.54, 0.78)
    ctx.set_line_width(3.0)
    ctx.stroke()

    # Cross shape (carved warm parchment-wood)
    arm_w = 34.0
    arm_l = 52.0
    hw = arm_w * 0.5
    ctx.move_to(cx - hw, cy - arm_l)
    ctx.line_to(cx + hw, cy - arm_l)
    ctx.line_to(cx + hw, cy - hw)
    ctx.line_to(cx + arm_l, cy - hw)
    ctx.line_to(cx + arm_l, cy + hw)
    ctx.line_to(cx + hw, cy + hw)
    ctx.line_to(cx + hw, cy + arm_l)
    ctx.line_to(cx - hw, cy + arm_l)
    ctx.line_to(cx - hw, cy + hw)
    ctx.line_to(cx - arm_l, cy + hw)
    ctx.line_to(cx - arm_l, cy - hw)
    ctx.line_to(cx - hw, cy - hw)
    ctx.close_path()
    cg = cairo.LinearGradient(cx, cy - arm_l, cx, cy + arm_l)
    if held:
        cg.add_color_stop_rgba(0, 1.00, 0.94, 0.74, 0.98)
        cg.add_color_stop_rgba(1, 0.88, 0.68, 0.34, 0.98)
    else:
        cg.add_color_stop_rgba(0, 0.98, 0.92, 0.76, 0.96)
        cg.add_color_stop_rgba(0.55, 0.84, 0.68, 0.44, 0.96)
        cg.add_color_stop_rgba(1, 0.64, 0.46, 0.25, 0.98)
    stroke_grad(ctx, cg, (0.26, 0.14, 0.05, 0.98), 3.6)

    # Center recessed wooden dimple
    ctx.arc(cx, cy, 11.0, 0, math.tau)
    dg = cairo.RadialGradient(cx, cy + 3, 2, cx, cy, 11)
    dg.add_color_stop_rgba(0, 0.56, 0.38, 0.20, 0.9)
    dg.add_color_stop_rgba(1, 0.86, 0.72, 0.48, 0.9)
    ctx.set_source(dg)
    ctx.fill_preserve()
    ctx.set_source_rgba(0.32, 0.18, 0.08, 0.6)
    ctx.set_line_width(2.0)
    ctx.stroke()

    # Crisp directional chevron triangles on all 4 arms
    for ang in (0, math.pi / 2, math.pi, 3 * math.pi / 2):
        ctx.save()
        ctx.translate(cx, cy)
        ctx.rotate(ang)
        ctx.move_to(42, 0); ctx.line_to(28, -9); ctx.line_to(28, 9); ctx.close_path()
        stroke_fill(ctx, (0.38, 0.22, 0.10, 0.90), (0.98, 0.90, 0.66, 0.65), 1.8)
        ctx.restore()
    ctx.restore()


def draw_dpad_dir_glow(ctx, angle_rad, held):
    """27..30: Glowing golden directional highlight overlay for Right/Left/Down/Up."""
    ctx.save()
    ctx.translate(64.0, 64.0)
    ctx.rotate(angle_rad)
    # Lit arm cap
    rounded_rect(ctx, 14, -15, 38, 30, 7)
    gg = cairo.LinearGradient(14, 0, 52, 0)
    gg.add_color_stop_rgba(0, 1.0, 0.92, 0.48, 0.78 if not held else 0.95)
    gg.add_color_stop_rgba(1, 1.0, 0.78, 0.20, 0.92 if not held else 1.0)
    stroke_grad(ctx, gg, (1.0, 0.98, 0.82, 0.95), 2.5)
    # Bright arrow
    ctx.move_to(44, 0); ctx.line_to(28, -10); ctx.line_to(28, 10); ctx.close_path()
    stroke_fill(ctx, (1.0, 0.99, 0.92, 1.0), (0.45, 0.25, 0.05, 0.95), 2.2)
    ctx.restore()


def draw_stick_tile(ctx, is_knob):
    """31: idle=Warm Oak Compass Ring Base (for BOTH L & R Sticks); held=Tactile Cream-Gold Thumb Knob!"""
    ctx.save()
    cx, cy = 64.0, 64.0
    if not is_knob:
        # ---- Analog Stick Base Ring (Carved Oak & Golden Compass Ticks) ----
        # Translucent warm backdrop disc
        bg = cairo.RadialGradient(cx, cy, 10, cx, cy, 57)
        bg.add_color_stop_rgba(0, 0.26, 0.16, 0.08, 0.34)
        bg.add_color_stop_rgba(0.76, 0.22, 0.13, 0.06, 0.56)
        bg.add_color_stop_rgba(1.0, 0.16, 0.09, 0.04, 0.72)
        ctx.arc(cx, cy, 56, 0, math.tau)
        ctx.set_source(bg)
        ctx.fill()

        # Outer dark + gold double rim
        ctx.arc(cx, cy, 56, 0, math.tau)
        ctx.set_source_rgba(0.24, 0.13, 0.05, 0.92)
        ctx.set_line_width(6.5)
        ctx.stroke()

        rim = cairo.LinearGradient(cx - 54, cy - 54, cx + 54, cy + 54)
        rim.add_color_stop_rgba(0, 1.00, 0.92, 0.64, 0.95)
        rim.add_color_stop_rgba(0.5, 0.86, 0.66, 0.32, 0.95)
        rim.add_color_stop_rgba(1, 0.60, 0.40, 0.18, 0.95)
        ctx.arc(cx, cy, 54, 0, math.tau)
        ctx.set_source(rim)
        ctx.set_line_width(4.2)
        ctx.stroke()

        # Subtle inner guide ring
        ctx.arc(cx, cy, 36, 0, math.tau)
        ctx.set_source_rgba(1.0, 0.94, 0.76, 0.28)
        ctx.set_line_width(2.0)
        ctx.stroke()

        # 4 golden compass chevron ticks (N, E, S, W)
        for ang in (0, math.pi / 2, math.pi, 3 * math.pi / 2):
            ctx.save()
            ctx.translate(cx, cy)
            ctx.rotate(ang)
            ctx.move_to(50, 0); ctx.line_to(42, -5.5); ctx.line_to(42, 5.5); ctx.close_path()
            ctx.set_source_rgba(1.0, 0.92, 0.62, 0.85)
            ctx.fill()
            ctx.restore()
    else:
        # ---- Analog Stick Tactile Thumb Knob (Warm Cream-Gold Medallion) ----
        r = 50.0
        # Drop shadow
        sh = cairo.RadialGradient(cx, cy + 5, r * 0.5, cx, cy + 5, r + 9)
        sh.add_color_stop_rgba(0, 0.12, 0.06, 0.02, 0.55)
        sh.add_color_stop_rgba(1, 0.12, 0.06, 0.02, 0.0)
        ctx.arc(cx, cy + 5, r + 9, 0, math.tau)
        ctx.set_source(sh)
        ctx.fill()

        # Outer dark rim
        ctx.arc(cx, cy, r + 2.5, 0, math.tau)
        ctx.set_source_rgba(0.24, 0.13, 0.05, 0.96)
        ctx.fill()

        # Golden metallic bevel
        rm = cairo.LinearGradient(cx - r, cy - r, cx + r, cy + r)
        rm.add_color_stop_rgba(0, 1.00, 0.94, 0.68, 1.0)
        rm.add_color_stop_rgba(0.55, 0.92, 0.72, 0.34, 1.0)
        rm.add_color_stop_rgba(1, 0.66, 0.42, 0.14, 1.0)
        ctx.arc(cx, cy, r, 0, math.tau)
        ctx.set_source(rm)
        ctx.fill()

        # Inner tactile dome
        ir = r - 7.5
        dg = cairo.RadialGradient(cx - 14, cy - 16, 4, cx, cy, ir * 1.15)
        dg.add_color_stop_rgba(0, 1.00, 0.99, 0.92, 1.0)
        dg.add_color_stop_rgba(0.55, 0.98, 0.88, 0.64, 1.0)
        dg.add_color_stop_rgba(1, 0.82, 0.60, 0.28, 1.0)
        ctx.arc(cx, cy, ir, 0, math.tau)
        ctx.set_source(dg)
        ctx.fill_preserve()
        ctx.set_source_rgba(0.42, 0.24, 0.08, 0.55)
        ctx.set_line_width(2.2)
        ctx.stroke()

        # Subtle center grip ring
        ctx.arc(cx, cy, 20, 0, math.tau)
        ctx.set_source_rgba(0.55, 0.34, 0.12, 0.35)
        ctx.set_line_width(2.5)
        ctx.stroke()
    ctx.restore()


# ---- Master Tile Table (32 tiles: 0..31) ----
# Each entry: (palette, badge_text, draw_fn) or ("special", fn)
TILES = [
    # Row-pair 0 (0..7): Tool Action Y variants
    ("tool", "Y", ic_hand),            # 0: Hand / Default
    ("tool", "Y", ic_watering_can),    # 1: Watering Can
    ("tool", "Y", ic_hoe),             # 2: Hoe
    ("tool", "Y", ic_hammer),          # 3: Hammer / Pickaxe
    ("tool", "Y", ic_axe),             # 4: Axe
    ("tool", "Y", ic_sickle),          # 5: Sickle
    ("tool", "Y", ic_fishing_rod),     # 6: Fishing Rod
    ("tool", "Y", ic_bug_net),         # 7: Bug Net
    # Row-pair 1 (8..15): More Y variants + Interact A variants + B + X
    ("tool", "Y", ic_seeds),           # 8: Seed Pouch
    ("tool", "Y", ic_animal_care),     # 9: Animal Care (Brush/Milker/Shears/Bell/Feed)
    ("tool", "Y", ic_carry_gift),      # 10: Carry / Eat / Rucksack Item
    ("interact", "A", ic_talk),        # 11: Talk / Interact (A)
    ("interact", "A", ic_confirm),     # 12: Confirm / OK in Menu/Title (A)
    ("interact", "A", ic_harvest_give),# 13: Give / Harvest while carrying (A)
    ("cancel", "B", ic_dash_cancel),   # 14: Dash / Cancel / Back (B)
    ("bag", "X", ic_bag),              # 15: Bag / Inventory (X)
    # Row-pair 2 (16..23): Shoulders, Triggers, System, Stick-Press
    ("sky", "L", lambda c: ic_chevron(c, -1)),  # 16: Quick Slot Prev (L)
    ("sky", "R", lambda c: ic_chevron(c, +1)),  # 17: Quick Slot Next (R)
    ("wood", "ZL", ic_swap),           # 18: Pocket Swap (ZL)
    ("bell", "ZR", ic_bell),           # 19: Doraemon Bell / Gadget (ZR)
    ("wood", "-", ic_map),             # 20: World Map (-)
    ("wood", "+", ic_journal),         # 21: Journal / System (+)
    ("sky", "RS", ic_camera),          # 22: Camera Reset (RS)
    ("sky", "LS", ic_sprint_ls),       # 23: Stick Press (LS)
    # Row-pair 3 (24..31): Drawer + Adaptive D-Pad + Adaptive Analog Sticks
    ("wood", "", ic_drawer_open),      # 24: Drawer Expand
    ("wood", "", ic_drawer_close),     # 25: Drawer Collapse
    ("special", None, draw_dpad_base),                         # 26: D-Pad Carved Wood Base
    ("special", None, lambda c, h: draw_dpad_dir_glow(c, 0.0, h)),            # 27: D-Pad Right Glow
    ("special", None, lambda c, h: draw_dpad_dir_glow(c, math.pi, h)),        # 28: D-Pad Left Glow
    ("special", None, lambda c, h: draw_dpad_dir_glow(c, math.pi * 0.5, h)),  # 29: D-Pad Down Glow
    ("special", None, lambda c, h: draw_dpad_dir_glow(c, -math.pi * 0.5, h)), # 30: D-Pad Up Glow
    ("special", None, draw_stick_tile),                        # 31: idle=Stick Base Ring, held=Stick Thumb Knob
]


def render_tile(idx, held):
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, TILE, TILE)
    ctx = cairo.Context(surf)
    ctx.set_antialias(cairo.ANTIALIAS_BEST)
    kind, badge, fn = TILES[idx]
    if kind == "special":
        fn(ctx, held)
    else:
        draw_medallion(ctx, kind, held)
        fn(ctx)
        draw_badge(ctx, badge)
    # Convert Cairo premultiplied BGRA -> straight RGBA PIL Image
    buf = bytes(surf.get_data())
    im = Image.frombuffer("RGBA", (TILE, TILE), buf, "raw", "BGRA", 0, 1).copy()
    # Un-premultiply alpha for crisp Vulkan SRC_ALPHA blending
    px = im.load()
    for y in range(TILE):
        for x in range(TILE):
            r, g, b, a = px[x, y]
            if 0 < a < 255:
                px[x, y] = (min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a), a)
    return im


def build_adaptive_atlas():
    atlas = Image.new("RGBA", (COLS * TILE, ROWS * TILE), (0, 0, 0, 0))
    for idx in range(len(TILES)):
        col = idx % COLS
        row_pair = idx // COLS
        im_idle = render_tile(idx, False)
        im_held = render_tile(idx, True)
        atlas.paste(im_idle, (col * TILE, (row_pair * 2) * TILE))
        atlas.paste(im_held, (col * TILE, (row_pair * 2 + 1) * TILE))
    out_path = os.path.join(ASSETS_OV, "adaptive.png")
    atlas.save(out_path, "PNG", optimize=True)
    print(f"Wrote {out_path} ({atlas.width}x{atlas.height}, {os.path.getsize(out_path)} bytes)")
    return atlas


def build_hud_preview(atlas):
    """Render a quick 1280x720 mock-farm screenshot preview so we can visually verify aesthetics."""
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    W, H = 1280, 720
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    ctx = cairo.Context(surf)
    # Cozy warm meadow backdrop
    g = cairo.LinearGradient(0, 0, 0, H)
    g.add_color_stop_rgb(0, 0.54, 0.78, 0.94)
    g.add_color_stop_rgb(0.45, 0.64, 0.84, 0.50)
    g.add_color_stop_rgb(1, 0.42, 0.64, 0.30)
    ctx.set_source(g)
    ctx.rectangle(0, 0, W, H)
    ctx.fill()
    bg = Image.frombuffer("RGBA", (W, H), bytes(surf.get_data()), "raw", "BGRA", 0, 1).copy()

    def paste_tile(idx, held, cx, cy, diam):
        col = idx % COLS
        row = (idx // COLS) * 2 + (1 if held else 0)
        tile = atlas.crop((col * TILE, row * TILE, (col + 1) * TILE, (row + 1) * TILE))
        tile = tile.resize((int(diam), int(diam)), Image.Resampling.LANCZOS)
        bg.alpha_composite(tile, (int(cx - diam / 2), int(cy - diam / 2)))

    # Left side: Stick Base + Knob, D-Pad, L, ZL, Minus
    paste_tile(31, False, 195, 515, 190)
    paste_tile(31, True, 212, 502, 86)
    paste_tile(26, False, 355, 610, 138)
    paste_tile(27, True, 355, 610, 138)
    paste_tile(16, False, 118, 285, 84)
    paste_tile(18, False, 118, 175, 84)
    paste_tile(20, False, 470, 62, 68)

    # Right side: Ergonomic Action Cluster (Watering Can Y, Talk A, Dash B, Bag X, R, ZR, +, R-Stick)
    paste_tile(1, False, 1135, 530, 146)   # Y (Watering Can)
    paste_tile(11, False, 975, 585, 116)   # A (Talk)
    paste_tile(14, False, 1020, 425, 106)  # B (Dash)
    paste_tile(15, False, 1160, 365, 106)  # X (Bag)
    paste_tile(17, False, 1162, 245, 84)   # R
    paste_tile(19, False, 1055, 245, 84)   # ZR (Doraemon Bell!)
    paste_tile(21, False, 810, 62, 68)     # + (Journal)
    paste_tile(31, False, 855, 595, 134)   # Right Stick Base
    paste_tile(31, True, 855, 595, 60)     # Right Stick Knob

    out = os.path.join(PREVIEW_DIR, "adaptive_hud_preview.png")
    bg.save(out)
    print(f"Wrote preview {out}")


if __name__ == "__main__":
    atlas = build_adaptive_atlas()
    build_hud_preview(atlas)
