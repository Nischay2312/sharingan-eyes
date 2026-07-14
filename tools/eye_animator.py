#!/usr/bin/env python3
"""
eye_animator.py -- generate dojutsu eye animations on the PC and export them
as looping GIFs (preview / sharing) or .eyv clips (board playback).

Unlike eye_studio.py (which crops EXISTING gifs/videos), this tool CREATES the
animation: a vector-rendered anime eye (skia) with a spinning dojutsu pattern,
natural blinks, gaze saccades and an optional spin flare -- all placed on a
deterministic timeline so the exported clip loops seamlessly.

  python tools/eye_animator.py

Preview is a fast low-quality proxy; exports render at 4x supersampling on a
background thread. Needs: skia-python, pillow, numpy (tools/requirements.txt).
"""

import os
import sys
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, colorchooser

from PIL import Image, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eye_render.geometry import EyeShape                     # noqa: E402
from eye_render.patterns import SCENES, DEFAULT_COLORS       # noqa: E402
from eye_render.timeline import Timeline                     # noqa: E402
from eye_render.pipeline import (RenderConfig, render_frame, render_loop,
                                 export_gif, export_eyv)     # noqa: E402

PREVIEW = 480          # preview canvas px (2x of the 240 proxy)


class EyeAnimator:
    def __init__(self, root):
        self.root = root
        root.title("Eye Animator -- generate dojutsu clips")
        self.playing = False
        self.t = 0.0
        self.color = None          # None = scene default
        self.photo = None
        self._exporting = False
        self._ready = False        # suppress refresh during UI construction
        self._build_ui()
        self._ready = True
        self._refresh()

    # ------------------------------------------------------------- vars --
    def _var(self, val, cb=True):
        v = tk.DoubleVar(value=val) if isinstance(val, float) else tk.IntVar(value=val)
        if cb:
            v.trace_add("write", lambda *_: self._refresh())
        return v

    def _build_ui(self):
        top = ttk.Frame(self.root, padding=6)
        top.grid(row=0, column=0, sticky="nsew")

        # ---------- left: controls ----------
        left = ttk.Frame(top)
        left.grid(row=0, column=0, sticky="ns", padx=(0, 8))
        r = 0

        ttk.Label(left, text="Scene").grid(row=r, column=0, sticky="w")
        self.scene = tk.StringVar(value="mangekyou")
        cb = ttk.Combobox(left, textvariable=self.scene, values=SCENES,
                          state="readonly", width=12)
        cb.grid(row=r, column=1, sticky="w"); r += 1
        cb.bind("<<ComboboxSelected>>", lambda e: self._scene_changed())

        ttk.Button(left, text="Pattern color…", command=self._pick_color)\
            .grid(row=r, column=0, columnspan=2, sticky="ew", pady=2); r += 1

        def sect(name):
            nonlocal r
            ttk.Separator(left).grid(row=r, column=0, columnspan=2,
                                     sticky="ew", pady=4); r += 1
            ttk.Label(left, text=name, font=("", 9, "bold"))\
                .grid(row=r, column=0, columnspan=2, sticky="w"); r += 1

        def slider(name, var, lo, hi, res=None):
            nonlocal r
            ttk.Label(left, text=name).grid(row=r, column=0, sticky="w")
            s = tk.Scale(left, variable=var, from_=lo, to=hi, orient="horizontal",
                         resolution=res or ((hi - lo) / 100), showvalue=True,
                         length=150)
            s.grid(row=r, column=1, sticky="w"); r += 1

        sect("Eye shape")
        self.v_anime = self._var(1.0)
        self.v_width = self._var(1.0)
        self.v_height = self._var(1.0)
        self.v_corner = self._var(1.0)
        self.v_pupil = self._var(0.16)
        slider("anime \u2194 round", self.v_anime, 1.0, 0.0)
        slider("width", self.v_width, 0.7, 1.15)
        slider("height", self.v_height, 0.6, 1.4)
        slider("corner sharp", self.v_corner, 0.5, 1.3)
        slider("pupil size", self.v_pupil, 0.08, 0.4)

        sect("Loop (seamless)")
        self.v_loop = self._var(7.0)
        self.v_revs = self._var(1)
        self.v_blinks = self._var(2)
        self.v_gazen = self._var(3)
        self.v_gazeamp = self._var(0.10)
        self.v_flare = tk.IntVar(value=1)
        self.v_flare.trace_add("write", lambda *_: self._refresh())
        self.v_seed = self._var(7)
        slider("loop seconds", self.v_loop, 3.0, 20.0, 0.5)
        slider("spin revs/loop", self.v_revs, -4, 4, 1)
        slider("blinks/loop", self.v_blinks, 0, 5, 1)
        slider("gaze moves", self.v_gazen, 0, 6, 1)
        slider("gaze amount", self.v_gazeamp, 0.0, 0.25)
        ttk.Checkbutton(left, text="spin flare", variable=self.v_flare)\
            .grid(row=r, column=0, sticky="w"); r += 1
        slider("random seed", self.v_seed, 0, 99, 1)

        sect("Export")
        self.v_fps = self._var(25)
        self.v_quality = self._var(95)
        slider("fps", self.v_fps, 10, 30, 1)
        slider("JPEG quality", self.v_quality, 70, 100, 1)
        ttk.Button(left, text="Export GIF…", command=lambda: self._export("gif"))\
            .grid(row=r, column=0, columnspan=2, sticky="ew", pady=2); r += 1
        ttk.Button(left, text="Export .eyv…", command=lambda: self._export("eyv"))\
            .grid(row=r, column=0, columnspan=2, sticky="ew"); r += 1
        self.progress = ttk.Progressbar(left, maximum=100)
        self.progress.grid(row=r, column=0, columnspan=2, sticky="ew", pady=4); r += 1
        self.status = ttk.Label(left, text="")
        self.status.grid(row=r, column=0, columnspan=2, sticky="w"); r += 1

        # ---------- right: preview ----------
        right = ttk.Frame(top)
        right.grid(row=0, column=1, sticky="nsew")
        self.canvas = tk.Canvas(right, width=PREVIEW, height=PREVIEW, bg="#000",
                                highlightthickness=1, highlightbackground="#444")
        self.canvas.grid(row=0, column=0, columnspan=3, pady=(0, 4))

        self.scrub = tk.Scale(right, from_=0.0, to=7.0, resolution=0.02,
                              orient="horizontal", length=PREVIEW,
                              command=lambda v: self._scrubbed(float(v)),
                              label="time (s)")
        self.scrub.grid(row=1, column=0, columnspan=3, sticky="ew")

        self.play_btn = ttk.Button(right, text="\u25b6 Play loop", command=self._toggle_play)
        self.play_btn.grid(row=2, column=0, sticky="w", pady=4)
        ttk.Label(right, text="preview is a fast proxy; exports render full quality")\
            .grid(row=2, column=1, columnspan=2, sticky="e")

    # ------------------------------------------------------------ config --
    def _shape(self):
        return EyeShape(anime=self.v_anime.get(), width=self.v_width.get(),
                        height=self.v_height.get(), corner_sharp=self.v_corner.get())

    def _timeline(self):
        return Timeline(loop_s=float(self.v_loop.get()),
                        spin_revs=int(self.v_revs.get()),
                        blinks=int(self.v_blinks.get()),
                        gaze_amp=float(self.v_gazeamp.get()),
                        gaze_moves=int(self.v_gazen.get()),
                        flare=bool(self.v_flare.get()),
                        seed=int(self.v_seed.get()))

    def _config(self, size, supersample):
        return RenderConfig(scene=self.scene.get(), color=self.color,
                            shape=self._shape(), size=size,
                            supersample=supersample,
                            pupil_ratio=float(self.v_pupil.get()),
                            fps=int(self.v_fps.get()),
                            timeline=self._timeline())

    # ----------------------------------------------------------- preview --
    def _refresh(self):
        if self._exporting or not self._ready:
            return
        try:
            loop_s = float(self.v_loop.get())
        except (tk.TclError, ValueError):
            return
        self.scrub.configure(to=loop_s)
        cfg = self._config(240, 1)          # fast proxy
        st = cfg.timeline.eval(self.t)
        im = render_frame(cfg, st).resize((PREVIEW, PREVIEW), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(im)
        self.canvas.create_image(0, 0, image=self.photo, anchor="nw")

    def _scrubbed(self, t):
        self.t = t
        if not self.playing:
            self._refresh()

    def _toggle_play(self):
        self.playing = not self.playing
        self.play_btn.configure(text="\u23f8 Pause" if self.playing else "\u25b6 Play loop")
        if self.playing:
            self._tick()

    def _tick(self):
        if not self.playing:
            return
        step = 1.0 / 15.0                    # ~15 fps proxy playback
        self.t = (self.t + step) % float(self.v_loop.get())
        self.scrub.set(self.t)
        self._refresh()
        self.root.after(int(step * 1000), self._tick)

    def _scene_changed(self):
        self.color = None                    # back to the scene default
        self._refresh()

    def _pick_color(self):
        cur = self.color or DEFAULT_COLORS.get(self.scene.get(), (214, 32, 34))
        rgb, _ = colorchooser.askcolor(color="#%02x%02x%02x" % cur)
        if rgb:
            self.color = tuple(int(c) for c in rgb)
            self._refresh()

    # ------------------------------------------------------------ export --
    def _export(self, kind):
        if self._exporting:
            return
        ext = ".gif" if kind == "gif" else ".eyv"
        path = filedialog.asksaveasfilename(
            defaultextension=ext,
            initialfile=f"{self.scene.get()}{ext}",
            filetypes=[(ext.upper()[1:], "*" + ext)])
        if not path:
            return
        self._exporting = True
        self.playing = False
        self.play_btn.configure(text="\u25b6 Play loop")
        self.status.configure(text="rendering…")
        cfg = self._config(240, 4)           # full quality
        quality = int(self.v_quality.get())

        def work():
            try:
                def prog(i, n):
                    self.root.after(0, lambda: self.progress.configure(
                        value=100.0 * i / n))
                frames = render_loop(cfg, progress=prog)
                if kind == "gif":
                    size = export_gif(frames, path, cfg.fps)
                else:
                    size = export_eyv(frames, path, cfg.fps, quality=quality)
                msg = f"saved {os.path.basename(path)} ({size/1024:.0f} KB, {len(frames)} frames)"
                self.root.after(0, lambda: self._export_done(msg))
            except Exception as e:                        # pragma: no cover
                self.root.after(0, lambda: self._export_done(f"export failed: {e}", err=True))

        threading.Thread(target=work, daemon=True).start()

    def _export_done(self, msg, err=False):
        self._exporting = False
        self.progress.configure(value=0)
        self.status.configure(text=msg)
        if err:
            messagebox.showerror("Export", msg)


def main():
    root = tk.Tk()
    try:
        root.tk.call("tk", "scaling", 1.25)
    except tk.TclError:
        pass
    EyeAnimator(root)
    root.mainloop()


if __name__ == "__main__":
    main()
