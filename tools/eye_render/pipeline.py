"""
Frame renderer + exporters.

Renders one FrameState into a supersampled skia surface and composites the
anime eye -- dark skin, heavy upper-lid shadow + lashes, sclera slivers, the
iris/pattern filling the opening, glints and glow -- then downsamples to the
240x240 clip size. Exports animated GIFs and .eyv clips (reusing make_anim's
container writer, JPEG at 4:4:4 chroma so red/black edges stay crisp).
"""

import io
import os
import sys
from dataclasses import dataclass, field

import skia
from PIL import Image

from .geometry import EyeShape, opening_path, upper_lid_edge, iris_geometry
from .patterns import (DEFAULT_COLORS, draw_tomoe_scene, draw_mangekyou,
                       draw_itachi, draw_rinnegan_fill, draw_byakugan_fill)
from .timeline import FrameState, Timeline

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import make_anim as M  # noqa: E402

FILL_SCENES = ("rinnegan", "tomoerin", "byakugan")


@dataclass
class RenderConfig:
    scene: str = "mangekyou"
    color: tuple = None
    shape: EyeShape = field(default_factory=EyeShape)
    size: int = 240
    supersample: int = 4
    pupil_ratio: float = 0.16     # pupil radius / fill radius
    fps: int = 25
    timeline: Timeline = field(default_factory=Timeline)
    skin_rgb: tuple = (10, 7, 8)

    def resolved_color(self):
        return self.color or DEFAULT_COLORS.get(self.scene, (220, 26, 28))


def render_frame(cfg: RenderConfig, st: FrameState) -> Image.Image:
    S = cfg.size * cfg.supersample
    surface = skia.Surface(S, S)
    canvas = surface.getCanvas()
    cx = cy = S / 2.0
    scale = S * 0.47                      # unit -> px (half eye width)
    rgb = cfg.resolved_color()
    is_fill = cfg.scene in FILL_SCENES

    # --- skin backdrop with a soft socket vignette
    canvas.clear(_c(cfg.skin_rgb))
    vign = skia.GradientShader.MakeRadial(
        (cx, cy), S * 0.6,
        [skia.Color(30, 20, 24), _c(cfg.skin_rgb)], [0.25, 1.0])
    canvas.drawRect(skia.Rect.MakeWH(S, S), _shader(vign))

    openness = st.openness
    if openness <= 0.02:
        _closed(canvas, cfg, cx, cy, scale)
        return _to_pil(surface, cfg.size)

    opening = opening_path(cfg.shape, openness, scale, cx, cy)
    icx, icy, r_fill, r_corner = iris_geometry(cfg.shape, openness, scale, cx, cy)
    icx += st.gaze_x * scale * 0.5
    icy += st.gaze_y * scale * 0.35

    # --- outer glow halo (bloom); rinnegan/purple glows harder
    glow_a = int((70 if is_fill else 40) + 120 * st.glow)
    glow = skia.Paint(AntiAlias=True)
    glow.setColor(_c(_mul(rgb, 1.1), glow_a))
    glow.setMaskFilter(skia.MaskFilter.MakeBlur(
        skia.kNormal_BlurStyle, S * (0.05 if is_fill else 0.035)))
    canvas.drawPath(opening, glow)

    # --- everything inside the lids
    canvas.save()
    canvas.clipPath(opening, doAntiAlias=True)

    if is_fill:
        pupil_r = r_fill * cfg.pupil_ratio
        if cfg.scene == "byakugan":
            draw_byakugan_fill(canvas, icx, icy, r_corner, rgb)
        else:
            draw_rinnegan_fill(canvas, icx, icy, r_corner, rgb, pupil_r,
                               phase=st.phase, drift=st.drift,
                               tomoe=6 if cfg.scene == "tomoerin" else 0)
    else:
        # sclera fills the opening; the coloured iris disc sits on top and
        # overflows top/bottom so only the corner slivers stay white.
        scl = skia.GradientShader.MakeLinear(
            [(cx - scale, cy), (cx + scale, cy)],
            [skia.Color(120, 110, 116), skia.Color(238, 236, 239),
             skia.Color(240, 238, 241), skia.Color(112, 102, 110)],
            [0.0, 0.24, 0.76, 1.0])
        canvas.drawPaint(_shader(scl))

        R = r_fill * 1.14
        pupil_r = R * 0.30
        shadow = skia.Paint(AntiAlias=True)
        shadow.setColor(skia.Color(0, 0, 0, 70))
        shadow.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, S * 0.012))
        canvas.drawCircle(icx, icy + S * 0.008, R * 1.03, shadow)
        if cfg.scene == "mangekyou":
            draw_mangekyou(canvas, icx, icy, R, st.phase, pupil_r * st.pupil_scale, rgb)
        elif cfg.scene == "itachi":
            draw_itachi(canvas, icx, icy, R, st.phase, pupil_r * st.pupil_scale, rgb)
        else:
            n = {"tomoe1": 1, "tomoe2": 2, "tomoe3": 3}.get(cfg.scene, 3)
            draw_tomoe_scene(canvas, icx, icy, R, n, st.phase,
                             pupil_r * st.pupil_scale, rgb)

    # specular streak (the wet-eye highlight) toward the outer-upper area
    _glint(canvas, icx, icy, r_fill, S)

    # upper-lid shadow cast into the eye (sells depth)
    lid_sh = skia.Paint(AntiAlias=True)
    lid_sh.setStyle(skia.Paint.kStroke_Style)
    lid_sh.setStrokeWidth(S * 0.045)
    lid_sh.setColor(skia.Color(0, 0, 0, 120))
    lid_sh.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, S * 0.022))
    canvas.drawPath(upper_lid_edge(cfg.shape, openness, scale, cx, cy), lid_sh)

    canvas.restore()

    # --- crisp lid line + lashes + brow fold (outside the clip)
    edge = skia.Paint(AntiAlias=True)
    edge.setStyle(skia.Paint.kStroke_Style)
    edge.setStrokeWidth(S * 0.02)
    edge.setStrokeCap(skia.Paint.kRound_Cap)
    edge.setColor(skia.Color(0, 0, 0, 255))
    canvas.drawPath(opening, edge)
    _lashes(canvas, cfg, openness, scale, cx, cy, S)

    fold = skia.Paint(AntiAlias=True)
    fold.setStyle(skia.Paint.kStroke_Style)
    fold.setStrokeWidth(S * 0.011)
    fold.setColor(skia.Color(70, 60, 66, 150))
    canvas.save()
    canvas.translate(0, -S * 0.05 * openness)
    canvas.drawPath(upper_lid_edge(cfg.shape, min(1.0, openness * 1.05),
                                   scale, cx, cy), fold)
    canvas.restore()

    return _to_pil(surface, cfg.size)


# ------------------------------------------------------------- helpers -----

def _c(rgb, a=255):
    return skia.Color(int(rgb[0]), int(rgb[1]), int(rgb[2]), a)


def _mul(rgb, k):
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


def _shader(sh):
    p = skia.Paint(AntiAlias=True)
    p.setShader(sh)
    return p


def _glint(canvas, ix, iy, r, S):
    gl = skia.Paint(AntiAlias=True)
    gl.setColor(skia.Color(255, 255, 255, 225))
    gl.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, S * 0.004))
    canvas.save()
    canvas.translate(ix + r * 0.55, iy - r * 0.5)
    canvas.rotate(-20)
    canvas.drawOval(skia.Rect.MakeLTRB(-r * 0.30, -r * 0.11, r * 0.30, r * 0.11), gl)
    canvas.restore()


def _lashes(canvas, cfg, openness, scale, cx, cy, S):
    """A few tapered lash strokes flicking off the outer upper corner."""
    from .geometry import metrics
    up_h, lo_h, peak_x, cl, cr = metrics(cfg.shape, openness)
    lash = skia.Paint(AntiAlias=True)
    lash.setStyle(skia.Paint.kStroke_Style)
    lash.setStrokeCap(skia.Paint.kRound_Cap)
    ox, oy = cx + cr[0] * scale, cy + cr[1] * scale
    for i, (dx, dy, w) in enumerate([(0.11, -0.02, 0.022),
                                     (0.14, -0.06, 0.02),
                                     (0.13, -0.11, 0.017)]):
        lash.setStrokeWidth(S * w)
        canvas.drawLine(ox, oy, ox + dx * scale, oy + dy * scale, lash)


def _closed(canvas, cfg, cx, cy, scale):
    ln = skia.Paint(AntiAlias=True)
    ln.setStyle(skia.Paint.kStroke_Style)
    ln.setStrokeWidth(scale * 0.03)
    ln.setStrokeCap(skia.Paint.kRound_Cap)
    ln.setColor(skia.Color(0, 0, 0, 255))
    path = skia.Path()
    path.moveTo(cx - scale, cy - scale * 0.02)
    path.cubicTo(cx - scale * 0.4, cy + scale * 0.05,
                 cx + scale * 0.4, cy + scale * 0.05, cx + scale, cy + scale * 0.03)
    canvas.drawPath(path, ln)


def _to_pil(surface, out_size) -> Image.Image:
    img = surface.makeImageSnapshot()
    arr = img.toarray(colorType=skia.ColorType.kRGBA_8888_ColorType)
    pil = Image.fromarray(arr, "RGBA").convert("RGB")
    if pil.width != out_size:
        pil = pil.resize((out_size, out_size), Image.LANCZOS)
    return pil


# --------------------------------------------------------------- loops -----

def render_loop(cfg: RenderConfig, progress=None):
    n = max(1, round(cfg.timeline.loop_s * cfg.fps))
    frames = []
    for i in range(n):
        frames.append(render_frame(cfg, cfg.timeline.eval(i / cfg.fps)))
        if progress:
            progress(i + 1, n)
    return frames


# --------------------------------------------------------------- export ----

def export_gif(frames, path, fps):
    ms = max(20, round(1000 / fps))
    frames[0].save(path, save_all=True, append_images=frames[1:],
                   duration=ms, loop=0, optimize=False)
    return os.path.getsize(path)


def _encode_jpeg_444(im, quality=95):
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, subsampling=0, optimize=True)
    return buf.getvalue()


def export_eyv(frames, path, fps, quality=95, fmt="mjpeg"):
    if fmt == "raw":
        blobs = [M.to_rgb565_le(f) for f in frames]
        return M.pack_eyv(path, blobs, fps, M.FMT_RAW, loop=True)
    blobs = [_encode_jpeg_444(f, quality) for f in frames]
    return M.pack_eyv(path, blobs, fps, M.FMT_MJPEG, loop=True)
