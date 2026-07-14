"""
eye_render -- offline, high-fidelity dojutsu eye animation renderer.

Vector-drawn with skia (AA paths, gradients, real blur) instead of porting the
ESP firmware's integer pixel math, so clips look like the reference GIFs:
sharp anime eyelids, glossy red iris, crisp pinwheel, specular glints, glow.

Modules:
  geometry  -- eyelid bezier silhouette, anime<->round interpolation
  patterns  -- the 8 dojutsu iris patterns (tomoe1/2/3, mangekyou, itachi,
               rinnegan, tomoerin, byakugan)
  timeline  -- deterministic, seamlessly-looping event timeline
  pipeline  -- frame renderer + GIF / .eyv export

Used by tools/eye_animator.py (GUI) and scriptable directly:

    from eye_render.pipeline import RenderConfig, render_loop, export_gif
    cfg = RenderConfig(scene="mangekyou")
    frames = render_loop(cfg)
    export_gif(frames, "mangekyou.gif", cfg.fps)
"""

from . import geometry, patterns, timeline, pipeline  # noqa: F401
