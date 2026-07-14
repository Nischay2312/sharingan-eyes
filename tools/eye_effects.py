#!/usr/bin/env python3
"""
eye_effects.py -- Photoshop-style image effects for eye clips.

process(im_rgb, params, t) applies a stack of adjustments to a PIL RGB image.
`t` is the normalised loop time (0..1) used by the time-based "animate" effects,
so the same params produce a moving result across a clip's frames.

Used by tools/eye_studio.py (live sliders) and can be called from scripts too.
"""

import math

import numpy as np
from PIL import Image, ImageEnhance, ImageFilter, ImageChops, ImageOps

# Default value for every parameter (also the GUI "reset" target).
DEFAULTS = dict(
    brightness=1.0, contrast=1.0, saturation=1.0, gamma=1.0, hue=0.0,
    glow=0.0, glow_radius=3.0, glow_thresh=160.0, sharpen=1.0, vignette=0.0,
    rg=1.0, gg=1.0, bg=1.0, tint=0.0,
    hue_cycle=0.0, spin=0.0, pulse=0.0, pulse_cycles=1.0,
    breathe=0.0, breathe_cycles=1.0,
)


def _gamma(im, g):
    if abs(g - 1.0) < 1e-3:
        return im
    lut = [min(255, int(255 * ((i / 255.0) ** (1.0 / max(0.05, g))))) for i in range(256)]
    return im.point(lut * 3)


def _hue_shift(im, deg):
    if abs(deg) < 1e-2:
        return im
    h, s, v = im.convert("HSV").split()
    shift = int(round(deg / 360.0 * 255)) % 256
    h = h.point(lambda x: (x + shift) % 256)
    return Image.merge("HSV", (h, s, v)).convert("RGB")


def _rgb_gain(im, rg, gg, bg):
    if rg == gg == bg == 1.0:
        return im
    r, g, b = im.split()
    r = r.point(lambda x: min(255, int(x * rg)))
    g = g.point(lambda x: min(255, int(x * gg)))
    b = b.point(lambda x: min(255, int(x * bg)))
    return Image.merge("RGB", (r, g, b))


def _tint(im, color, strength):
    if strength <= 0:
        return im
    mult = ImageChops.multiply(im, Image.new("RGB", im.size, tuple(int(c) for c in color)))
    return Image.blend(im, mult, min(1.0, strength))


def _glow(im, amount, radius, thresh):
    if amount <= 0:
        return im
    hi = im.point(lambda x: x if x >= thresh else 0)
    if radius > 0:
        hi = hi.filter(ImageFilter.GaussianBlur(radius))
    hi = ImageEnhance.Brightness(hi).enhance(amount)
    return ImageChops.screen(im, hi)


def _vignette(im, amount):
    if amount <= 0:
        return im
    w, h = im.size
    yy, xx = np.ogrid[:h, :w]
    cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
    r = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / math.hypot(cx, cy)
    mask = 1.0 - amount * np.clip(r, 0, 1) ** 2
    arr = np.asarray(im).astype(np.float32) * mask[..., None]
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def _spin(im, deg):
    if abs(deg) < 1e-2:
        return im
    return im.rotate(deg, resample=Image.BILINEAR, expand=False)


def _breathe(im, scale):
    if abs(scale - 1.0) < 1e-3:
        return im
    w, h = im.size
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    z = im.resize((nw, nh), Image.LANCZOS)
    out = Image.new("RGB", (w, h), (0, 0, 0))
    out.paste(z, ((w - nw) // 2, (h - nh) // 2))
    if scale > 1.0:  # zoomed in: crop back to centre
        l, tp = (nw - w) // 2, (nh - h) // 2
        return z.crop((l, tp, l + w, tp + h))
    return out


def process(im, p, t=0.0):
    """Apply the effect stack. p: dict (see DEFAULTS). t: loop time 0..1."""
    g = lambda k: p.get(k, DEFAULTS[k])

    # --- geometry / animated motion ---
    if g("spin"):
        im = _spin(im, g("spin") * 360.0 * t)
    if g("breathe"):
        im = _breathe(im, 1.0 + g("breathe") * math.sin(2 * math.pi * g("breathe_cycles") * t))

    # --- tone / color ---
    b = g("brightness")
    if g("pulse"):
        b *= 1.0 + g("pulse") * math.sin(2 * math.pi * g("pulse_cycles") * t)
    if abs(b - 1.0) > 1e-3:
        im = ImageEnhance.Brightness(im).enhance(b)
    if g("contrast") != 1.0:
        im = ImageEnhance.Contrast(im).enhance(g("contrast"))
    im = _hue_shift(im, g("hue") + g("hue_cycle") * 360.0 * t)
    if g("saturation") != 1.0:
        im = ImageEnhance.Color(im).enhance(g("saturation"))
    im = _rgb_gain(im, g("rg"), g("gg"), g("bg"))
    im = _tint(im, p.get("tint_color", (255, 255, 255)), g("tint"))
    im = _gamma(im, g("gamma"))
    if g("sharpen") != 1.0:
        im = ImageEnhance.Sharpness(im).enhance(g("sharpen"))
    im = _glow(im, g("glow"), g("glow_radius"), g("glow_thresh"))
    im = _vignette(im, g("vignette"))
    if p.get("invert"):
        im = ImageOps.invert(im)
    return im
