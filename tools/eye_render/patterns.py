"""
Dojutsu iris patterns, drawn as anti-aliased skia vector paths.

Two families:
  * DISC scenes (tomoe1/2/3, mangekyou, itachi) -- a bounded coloured iris disc
    of radius `R` with the pattern on top and a pupil.
  * FILL scenes (rinnegan, tomoerin) -- concentric rings that fill the WHOLE
    eye opening out to `R_corner` (clipped by the lids), a tiny central pupil,
    and (tomoerin) six tomoe seated on one ring. Matches the reference art.

`phase` is the pattern rotation in radians. Colors are (r, g, b) 0..255.
Scene names match the firmware (eye_params.h / eye_scene_id_t).
"""

import math

import skia

SCENES = ["tomoe1", "tomoe2", "tomoe3", "mangekyou",
          "itachi", "rinnegan", "tomoerin", "byakugan"]

DEFAULT_COLORS = {
    "tomoe1":   (220, 26, 28),
    "tomoe2":   (220, 26, 28),
    "tomoe3":   (220, 26, 28),
    "mangekyou": (208, 20, 26),
    "itachi":   (208, 20, 26),
    "rinnegan": (150, 120, 205),
    "tomoerin": (150, 120, 205),
    "byakugan": (228, 230, 246),
}


def _color(rgb, a=255):
    return skia.Color(int(rgb[0]), int(rgb[1]), int(rgb[2]), a)


def _fill(color):
    p = skia.Paint(AntiAlias=True)
    p.setColor(color)
    return p


def _scale_rgb(rgb, k):
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


# --------------------------------------------------------------- iris base --

def draw_iris_disc(canvas, ix, iy, R, rgb):
    """Radial-gradient iris disc + dark limbal ring (disc scenes)."""
    shader = skia.GradientShader.MakeRadial(
        (ix - R * 0.16, iy - R * 0.20), R * 1.22,
        [_color(_scale_rgb(rgb, 1.30)), _color(rgb), _color(_scale_rgb(rgb, 0.40))],
        [0.0, 0.55, 1.0])
    paint = skia.Paint(AntiAlias=True)
    paint.setShader(shader)
    canvas.drawCircle(ix, iy, R, paint)

    ring = skia.Paint(AntiAlias=True)
    ring.setStyle(skia.Paint.kStroke_Style)
    ring.setStrokeWidth(R * 0.085)
    ring.setColor(_color(_scale_rgb(rgb, 0.16)))
    canvas.drawCircle(ix, iy, R * 0.97, ring)

    sheen = skia.GradientShader.MakeLinear(
        [(ix, iy - R), (ix, iy + R)],
        [skia.Color(255, 255, 255, 40), skia.Color(255, 255, 255, 0)],
        [0.0, 0.55])
    sp = skia.Paint(AntiAlias=True)
    sp.setShader(sheen)
    canvas.drawCircle(ix, iy, R, sp)


def draw_pupil(canvas, ix, iy, r, rgb=(6, 3, 5)):
    canvas.drawCircle(ix, iy, r, _fill(_color(rgb)))
    hi = skia.Paint(AntiAlias=True)
    hi.setColor(skia.Color(255, 255, 255, 150))
    canvas.drawCircle(ix - r * 0.3, iy - r * 0.35, r * 0.22, hi)


# ------------------------------------------------------------------- tomoe --

def _draw_tomoe(canvas, paint, orbit, head_r, sweep_deg=150.0):
    """One fat comma at angle 0 on a circle of radius `orbit`: a round head
    plus a tail that curls along the orbit to a point."""
    canvas.drawCircle(orbit, 0, head_r, paint)
    steps = 30
    sweep = math.radians(sweep_deg)
    outer, inner = [], []
    for i in range(steps + 1):
        t = i / steps
        ang = -t * sweep
        w = head_r * (1.0 - t) ** 0.9              # taper to a point
        rc = orbit + head_r * 0.30 * math.sin(t * math.pi)  # curl in(+)/out
        ca, sa = math.cos(ang), math.sin(ang)
        outer.append(((rc + w) * ca, (rc + w) * sa))
        inner.append(((rc - w) * ca, (rc - w) * sa))
    tail = skia.Path()
    tail.moveTo(*outer[0])
    for p in outer[1:]:
        tail.lineTo(*p)
    for p in reversed(inner):
        tail.lineTo(*p)
    tail.close()
    canvas.drawPath(tail, paint)


def draw_tomoe_scene(canvas, ix, iy, R, n, phase, pupil_r, rgb):
    draw_iris_disc(canvas, ix, iy, R, rgb)
    orbit = (pupil_r + R * 0.94) / 2.0
    head_r = R * 0.15
    ring = skia.Paint(AntiAlias=True)
    ring.setStyle(skia.Paint.kStroke_Style)
    ring.setStrokeWidth(R * 0.028)
    ring.setColor(skia.Color(18, 4, 6, 230))
    canvas.drawCircle(ix, iy, orbit, ring)
    black = _fill(skia.Color(10, 2, 4, 255))
    for k in range(n):
        canvas.save()
        canvas.translate(ix, iy)
        canvas.rotate(math.degrees(phase) + k * 360.0 / n)
        _draw_tomoe(canvas, black, orbit, head_r, sweep_deg=118.0)
        canvas.restore()
    draw_pupil(canvas, ix, iy, pupil_r)


# --------------------------------------------------------------- mangekyou --

def draw_mangekyou(canvas, ix, iy, R, phase, pupil_r, rgb):
    draw_iris_disc(canvas, ix, iy, R, rgb)
    black = _fill(skia.Color(8, 2, 4, 255))
    canvas.drawCircle(ix, iy, pupil_r * 1.05, black)
    for k in range(3):
        canvas.save()
        canvas.translate(ix, iy)
        canvas.rotate(math.degrees(phase) + k * 120.0)
        r0, r1 = pupil_r, R * 0.95
        blade = skia.Path()
        blade.moveTo(r0 * 0.15, -r0 * 0.98)
        blade.cubicTo(r1 * 0.34, -r1 * 0.66, r1 * 0.78, -r1 * 0.52,
                      r1 * 0.99, -r1 * 0.12)
        blade.cubicTo(r1 * 0.66, -r1 * 0.24, r1 * 0.38, -r1 * 0.10,
                      r0 * 0.95, r0 * 0.32)
        blade.close()
        canvas.drawPath(blade, black)
        canvas.restore()
    ring = skia.Paint(AntiAlias=True)
    ring.setStyle(skia.Paint.kStroke_Style)
    ring.setStrokeWidth(R * 0.05)
    ring.setColor(skia.Color(8, 2, 4, 255))
    canvas.drawCircle(ix, iy, R * 0.60, ring)


def draw_itachi(canvas, ix, iy, R, phase, pupil_r, rgb):
    draw_iris_disc(canvas, ix, iy, R, rgb)
    black = _fill(skia.Color(8, 2, 4, 255))
    canvas.drawCircle(ix, iy, pupil_r * 1.1, black)
    for k in range(3):
        canvas.save()
        canvas.translate(ix, iy)
        canvas.rotate(math.degrees(phase) + k * 120.0)
        r1 = R * 0.88
        blade = skia.Path()
        blade.moveTo(-pupil_r * 0.35, -pupil_r * 1.0)
        blade.cubicTo(r1 * 0.30, -r1 * 0.86, r1 * 0.72, -r1 * 0.58,
                      r1 * 0.74, -r1 * 0.22)
        blade.cubicTo(r1 * 0.52, -r1 * 0.34, r1 * 0.26, -r1 * 0.34,
                      pupil_r * 0.65, -pupil_r * 0.78)
        blade.close()
        canvas.drawPath(blade, black)
        canvas.restore()


# ---------------------------------------------------------------- rinnegan --

def draw_rinnegan_fill(canvas, ix, iy, R_corner, rgb, pupil_r,
                       phase=0.0, drift=0.0, tomoe=0, rings=7):
    """Concentric rings filling the whole eye opening (caller has clipped to
    the lids), a small solid pupil, and optional tomoe on ring #2.

    `R_corner` is the reach to the sharp corners; rings are evenly spaced from
    the pupil out to there so they run edge-to-edge like the reference."""
    # Pale lavender wash behind the rings (brighter centre).
    wash = skia.GradientShader.MakeRadial(
        (ix, iy), R_corner,
        [_color(_scale_rgb(rgb, 1.55)), _color(_scale_rgb(rgb, 1.15)),
         _color(rgb)],
        [0.0, 0.5, 1.0])
    wp = skia.Paint(AntiAlias=True)
    wp.setShader(wash)
    canvas.drawCircle(ix, iy, R_corner, wp)

    ring = skia.Paint(AntiAlias=True)
    ring.setStyle(skia.Paint.kStroke_Style)
    ring.setStrokeWidth(R_corner * 0.016)
    ring.setColor(_color(_scale_rgb(rgb, 0.30)))

    gap = (R_corner - pupil_r) / rings
    for i in range(1, rings + 1):
        r = pupil_r + ((i + drift) % (rings + 1e-6)) * gap
        if pupil_r * 0.6 < r <= R_corner:
            canvas.drawCircle(ix, iy, r, ring)

    if tomoe:
        orbit = pupil_r + 2 * gap        # seat them on the 2nd ring
        head = gap * 0.34
        black = _fill(skia.Color(14, 6, 22, 255))
        for k in range(tomoe):
            canvas.save()
            canvas.translate(ix, iy)
            canvas.rotate(math.degrees(phase) + k * 360.0 / tomoe)
            _draw_tomoe(canvas, black, orbit, head, sweep_deg=78.0)
            canvas.restore()

    draw_pupil(canvas, ix, iy, pupil_r, rgb=(10, 6, 16))


# ---------------------------------------------------------------- byakugan --

def draw_byakugan_fill(canvas, ix, iy, R_corner, rgb):
    sh = skia.GradientShader.MakeRadial(
        (ix, iy), R_corner,
        [_color(_scale_rgb(rgb, 1.03)), _color(_scale_rgb(rgb, 0.9)),
         _color(_scale_rgb(rgb, 0.66))],
        [0.0, 0.7, 1.0])
    p = skia.Paint(AntiAlias=True)
    p.setShader(sh)
    canvas.drawCircle(ix, iy, R_corner, p)
    vein = skia.Paint(AntiAlias=True)
    vein.setStyle(skia.Paint.kStroke_Style)
    vein.setStrokeWidth(R_corner * 0.008)
    vein.setColor(skia.Color(150, 150, 190, 55))
    for k in range(16):
        a = k * math.tau / 16 + 0.2
        canvas.drawLine(ix + math.cos(a) * R_corner * 0.3,
                        iy + math.sin(a) * R_corner * 0.3,
                        ix + math.cos(a) * R_corner * 0.95,
                        iy + math.sin(a) * R_corner * 0.95, vein)
