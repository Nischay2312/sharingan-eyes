"""
Eyelid geometry: the sharp anime eye opening as skia bezier paths.

The reference art is a **flat, wide, sharply-pointed almond** (roughly 3:1),
NOT a rounded human almond. The upper lid rises quickly near the inner corner
to a peak and then declines in a near-straight slash to the outer corner; the
lower lid is a shallow curve. Both lids meet at hard points.

`anime` interpolates between a rounder human eye (0.0) and this sharp anime
silhouette (1.0). Coordinates are unit space, x in [-1, +1], centred on (0,0),
+y down. `openness` (0..1) scales the vertical gap: 0 = closed, 1 = fully open.
"""

from dataclasses import dataclass

import skia


@dataclass
class EyeShape:
    anime: float = 1.0        # 0 = round human, 1 = sharp anime
    width: float = 1.0        # horizontal half-width multiplier
    height: float = 1.0       # vertical opening multiplier
    corner_sharp: float = 1.0 # tangent flatness at the corners
    upper_bias: float = 0.5   # override upper-lid peak x (0.5 = auto)


def _lerp(a, b, t):
    return a + (b - a) * t


def metrics(shape: EyeShape, openness: float):
    """Return (up_h, lo_h, peak_x, corner_l, corner_r) in unit space.
    up_h/lo_h are the lid peak heights above/below the centre line."""
    a = max(0.0, min(1.0, shape.anime))
    w = shape.width

    # A balanced sharp almond (like tomoe_rinnegan.png): ~2.5:1 anime, taller
    # and rounder at anime=0. Height is the peak arch above/below the centre.
    up_h = _lerp(0.62, 0.48, a) * shape.height * openness
    lo_h = _lerp(0.54, 0.34, a) * shape.height * openness

    droop = _lerp(0.0, 0.06, a)       # slight downward tilt of the outer corner
    cl = (-w, -droop)                 # inner corner (screen-left)
    cr = (+w, +droop)                 # outer corner
    bias = shape.upper_bias if shape.upper_bias != 0.5 else _lerp(0.5, 0.44, a)
    peak_x = _lerp(cl[0], cr[0], bias)
    return up_h, lo_h, peak_x, cl, cr


def _lids(shape, openness):
    up_h, lo_h, peak_x, cl, cr = metrics(shape, openness)
    a = max(0.0, min(1.0, shape.anime))

    # Upper lid: smooth arch peaking a touch inside centre, then a slightly
    # longer, flatter decline to the sharp outer corner (subtle anime slash).
    up1 = (_lerp(cl[0], peak_x, 0.55), -up_h * _lerp(1.30, 1.42, a))
    up2 = (_lerp(peak_x, cr[0], 0.42), -up_h * _lerp(1.28, 1.18, a))

    # Lower lid: shallow symmetric-ish arc, deepest just past centre.
    lo1 = (_lerp(cl[0], cr[0], 0.32), lo_h * 1.30)
    lo2 = (_lerp(cl[0], cr[0], 0.66), lo_h * 1.26)
    return cl, cr, up1, up2, lo1, lo2


def _mapper(scale, cx, cy):
    return lambda p: (cx + p[0] * scale, cy + p[1] * scale)


def opening_path(shape: EyeShape, openness: float, scale: float,
                 cx: float, cy: float) -> skia.Path:
    cl, cr, up1, up2, lo1, lo2 = _lids(shape, openness)
    pt = _mapper(scale, cx, cy)
    path = skia.Path()
    path.moveTo(*pt(cl))
    path.cubicTo(*pt(up1), *pt(up2), *pt(cr))
    path.cubicTo(*pt(lo2), *pt(lo1), *pt(cl))
    path.close()
    return path


def upper_lid_edge(shape: EyeShape, openness: float, scale: float,
                   cx: float, cy: float) -> skia.Path:
    cl, cr, up1, up2, _, _ = _lids(shape, openness)
    pt = _mapper(scale, cx, cy)
    path = skia.Path()
    path.moveTo(*pt(cl))
    path.cubicTo(*pt(up1), *pt(up2), *pt(cr))
    return path


def iris_geometry(shape: EyeShape, openness: float, scale: float,
                  cx: float, cy: float):
    """Where the iris sits and how big it is to fill the opening vertically.
    Returns (icx, icy, r_fill, r_corner):
      icy      -- vertical centre of the opening (midway between the lids)
      r_fill   -- radius that just fills top/bottom (disc scenes overflow it a
                  touch so only side slivers of sclera show)
      r_corner -- radius that reaches the sharp corners (rinnegan rings fill
                  the whole eye out to here)."""
    up_h, lo_h, _, cl, cr = metrics(shape, openness)
    icy = cy + (lo_h - up_h) * 0.5 * scale
    r_fill = (up_h + lo_h) * 0.5 * scale
    r_corner = shape.width * scale * 0.98
    return cx, icy, r_fill, r_corner
