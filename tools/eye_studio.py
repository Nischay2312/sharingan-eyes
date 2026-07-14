#!/usr/bin/env python3
"""
eye_studio.py -- visual crop/preview tool for building eye-board .eyv clips.

Open a GIF/video, scrub frames, drag a square crop box (with the round-display
circle drawn in), watch the live "what the panel shows" preview, then export the
.eyv straight to disk.

  python tools/eye_studio.py [optional_input.gif]

Controls on the left (source) image:
  - drag inside the box ......... move it
  - mouse wheel ................. zoom the box in/out
  - the box is always square = the 240x240 display; the circle is the visible area

Needs: pillow, numpy (see tools/requirements.txt). Tkinter ships with Python.
"""

import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, colorchooser

from PIL import Image, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_anim as M      # noqa: E402
import eye_effects as FX   # noqa: E402

SRC_CANVAS = 460          # source preview area (px)
PREV_CANVAS = M.W * 2     # device preview shown at 2x (480)


class EyeStudio:
    def __init__(self, root, initial=None):
        self.root = root
        root.title("Eye Studio -- crop & build .eyv")
        self.frames = []
        self.durations = []
        self.cur = 0
        self.cx = self.cy = 100.0   # crop centre in source px
        self.s = 100.0              # crop side in source px
        self.in_f = 0               # trim in/out frame indices
        self.out_f = 0
        self.fx = {}                # effect name -> Tk var
        self._fx_def = {}           # effect name -> default (for reset)
        self.tint_color = (255, 255, 255)
        self._drag = None
        self.src_photo = None
        self.prev_photo = None
        self._build_ui()
        if initial:
            self._load(initial)

    # ---------- UI ----------
    def _build_ui(self):
        top = ttk.Frame(self.root, padding=6)
        top.grid(row=0, column=0, sticky="nsew")

        ttk.Button(top, text="Open GIF/Video…", command=self._open).grid(row=0, column=0, sticky="w")
        self.info = ttk.Label(top, text="no file")
        self.info.grid(row=0, column=1, columnspan=4, sticky="w", padx=8)

        # canvases
        self.src = tk.Canvas(top, width=SRC_CANVAS, height=SRC_CANVAS, bg="#222",
                             highlightthickness=1, highlightbackground="#444")
        self.src.grid(row=1, column=0, columnspan=3, pady=6)
        self.prev = tk.Canvas(top, width=PREV_CANVAS, height=PREV_CANVAS, bg="#000",
                              highlightthickness=1, highlightbackground="#444")
        self.prev.grid(row=1, column=3, columnspan=2, pady=6, padx=(8, 0))

        self.src.bind("<ButtonPress-1>", self._press)
        self.src.bind("<B1-Motion>", self._drag_move)
        self.src.bind("<MouseWheel>", self._wheel)        # Windows
        self.src.bind("<Button-4>", lambda e: self._zoom(1.1))   # Linux
        self.src.bind("<Button-5>", lambda e: self._zoom(0.9))

        # frame scrubber
        self.frame_var = tk.IntVar(value=0)
        self.scrub = ttk.Scale(top, from_=0, to=0, orient="horizontal",
                               command=self._on_scrub)
        self.scrub.grid(row=2, column=0, columnspan=5, sticky="ew", pady=(4, 2))

        # trim timeline (green = kept range, white line = current frame)
        self.tl = tk.Canvas(top, height=16, bg="#333", highlightthickness=0)
        self.tl.grid(row=3, column=0, columnspan=5, sticky="ew", pady=(0, 4))
        self.tl.bind("<Button-1>", self._tl_click)
        self.tl.bind("<B1-Motion>", self._tl_click)

        # trim controls
        tf = ttk.Frame(top)
        tf.grid(row=4, column=0, columnspan=5, sticky="ew", pady=(0, 6))
        ttk.Button(tf, text="Set In  [i]", command=self._set_in).grid(row=0, column=0)
        ttk.Button(tf, text="Set Out [o]", command=self._set_out).grid(row=0, column=1, padx=4)
        ttk.Button(tf, text="Reset trim", command=self._reset_trim).grid(row=0, column=2)
        self.trim_lbl = ttk.Label(tf, text="")
        self.trim_lbl.grid(row=0, column=3, sticky="w", padx=10)
        self.root.bind("i", lambda e: self._set_in())
        self.root.bind("o", lambda e: self._set_out())

        # top controls
        c = ttk.Frame(top)
        c.grid(row=5, column=0, columnspan=5, sticky="ew")
        self.fps = tk.IntVar(value=20)
        self.quality = tk.IntVar(value=90)
        self.fmt = tk.StringVar(value="mjpeg")
        self.mask = tk.BooleanVar(value=True)
        self.invert_var = tk.BooleanVar(value=False)
        self._spin(c, "FPS", self.fps, 1, 30, 0)
        self._spin(c, "JPEG quality", self.quality, 30, 100, 1)
        ttk.Label(c, text="Format").grid(row=0, column=4, sticky="e")
        ttk.Combobox(c, textvariable=self.fmt, values=["mjpeg", "raw"], width=7,
                     state="readonly").grid(row=0, column=5, sticky="w", padx=4)
        ttk.Checkbutton(c, text="circle mask", variable=self.mask,
                        command=self._refresh_preview).grid(row=0, column=6, padx=8)
        ttk.Button(c, text="Reset effects", command=self._reset_fx).grid(row=0, column=7, padx=8)

        # effects notebook (live preview updates on any change)
        self._build_effects(top, row=6)

        # output + export
        out = ttk.Frame(top)
        out.grid(row=7, column=0, columnspan=5, sticky="ew", pady=(8, 0))
        self.outpath = tk.StringVar(
            value=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "anim.eyv")))
        ttk.Label(out, text="Output").grid(row=0, column=0)
        ttk.Entry(out, textvariable=self.outpath, width=52).grid(row=0, column=1, padx=4)
        ttk.Button(out, text="…", width=3, command=self._browse_out).grid(row=0, column=2)
        ttk.Button(out, text="Export .eyv", command=self._export).grid(row=0, column=3, padx=8)
        self.status = ttk.Label(out, text="")
        self.status.grid(row=1, column=0, columnspan=4, sticky="w", pady=(4, 0))

    def _spin(self, parent, label, var, lo, hi, col):
        ttk.Label(parent, text=label).grid(row=0, column=col * 2, sticky="e")
        ttk.Spinbox(parent, from_=lo, to=hi, textvariable=var, width=6,
                    command=self._refresh_preview).grid(row=0, column=col * 2 + 1, sticky="w", padx=4)

    def _slider(self, parent, label, var, lo, hi, col):
        ttk.Label(parent, text=label).grid(row=1, column=col * 2, sticky="e", pady=4)
        ttk.Scale(parent, from_=lo, to=hi, variable=var, orient="horizontal", length=150,
                  command=lambda e: self._refresh_preview()).grid(row=1, column=col * 2 + 1, sticky="w", padx=4)

    # ---------- effects panel ----------
    def _fxrow(self, tab, row, label, key, lo, hi, default):
        v = tk.DoubleVar(value=default)
        self.fx[key] = v
        self._fx_def[key] = default
        ttk.Label(tab, text=label).grid(row=row, column=0, sticky="e", padx=4, pady=1)
        ttk.Scale(tab, from_=lo, to=hi, variable=v, orient="horizontal", length=180,
                  command=lambda e: self._refresh_preview()).grid(row=row, column=1, padx=4)

    def _fxrow_labeled(self, tab, row, label, key, lo, hi, default, fmt="{:.0f}"):
        """Like _fxrow but shows a live value label -- useful for numeric transforms."""
        v = tk.DoubleVar(value=default)
        self.fx[key] = v
        self._fx_def[key] = default
        val_lbl = ttk.Label(tab, text=fmt.format(default), width=6, anchor="w")
        def _upd(e=None):
            val_lbl.config(text=fmt.format(v.get()))
            self._refresh_preview()
        ttk.Label(tab, text=label).grid(row=row, column=0, sticky="e", padx=4, pady=1)
        ttk.Scale(tab, from_=lo, to=hi, variable=v, orient="horizontal", length=180,
                  command=_upd).grid(row=row, column=1, padx=4)
        val_lbl.grid(row=row, column=2, sticky="w", padx=2)

    def _build_effects(self, parent, row):
        nb = ttk.Notebook(parent)
        nb.grid(row=row, column=0, columnspan=5, sticky="ew", pady=6)

        color = ttk.Frame(nb); nb.add(color, text="Color")
        self._fxrow(color, 0, "Brightness", "brightness", 0.0, 2.0, 1.0)
        self._fxrow(color, 1, "Contrast", "contrast", 0.0, 2.0, 1.0)
        self._fxrow(color, 2, "Saturation", "saturation", 0.0, 2.5, 1.0)
        self._fxrow(color, 3, "Gamma", "gamma", 0.4, 2.5, 1.0)
        self._fxrow(color, 4, "Hue (deg)", "hue", -180.0, 180.0, 0.0)

        tone = ttk.Frame(nb); nb.add(tone, text="Glow / Tone")
        self._fxrow(tone, 0, "Glow", "glow", 0.0, 2.0, 0.0)
        self._fxrow(tone, 1, "Glow radius", "glow_radius", 0.0, 8.0, 3.0)
        self._fxrow(tone, 2, "Glow thresh", "glow_thresh", 80.0, 240.0, 160.0)
        self._fxrow(tone, 3, "Sharpen", "sharpen", 0.0, 3.0, 1.0)
        self._fxrow(tone, 4, "Vignette", "vignette", 0.0, 1.0, 0.0)

        ch = ttk.Frame(nb); nb.add(ch, text="Channels / Tint")
        self._fxrow(ch, 0, "Red gain", "rg", 0.0, 2.0, 1.0)
        self._fxrow(ch, 1, "Green gain", "gg", 0.0, 2.0, 1.0)
        self._fxrow(ch, 2, "Blue gain", "bg", 0.0, 2.0, 1.0)
        self._fxrow(ch, 3, "Tint amount", "tint", 0.0, 1.0, 0.0)
        ttk.Button(ch, text="Tint color…", command=self._pick_tint).grid(row=4, column=0, pady=3)
        ttk.Checkbutton(ch, text="Invert", variable=self.invert_var,
                        command=self._refresh_preview).grid(row=4, column=1, sticky="w")

        an = ttk.Frame(nb); nb.add(an, text="Animate")
        self._fxrow(an, 0, "Hue cycle (turns)", "hue_cycle", -2.0, 2.0, 0.0)
        self._fxrow(an, 1, "Spin (turns)", "spin", -2.0, 2.0, 0.0)
        self._fxrow(an, 2, "Pulse depth", "pulse", 0.0, 1.0, 0.0)
        self._fxrow(an, 3, "Pulse cycles", "pulse_cycles", 0.5, 4.0, 1.0)
        self._fxrow(an, 4, "Breathe depth", "breathe", 0.0, 0.4, 0.0)
        self._fxrow(an, 5, "Breathe cycles", "breathe_cycles", 0.5, 4.0, 1.0)

        xf = ttk.Frame(nb); nb.add(xf, text="Transform")
        ttk.Label(xf, text="Baked into every frame — no device CPU cost.",
                  foreground="#888").grid(row=0, column=0, columnspan=3, sticky="w", padx=4, pady=(4, 2))
        self._fxrow_labeled(xf, 1, "Rotate (deg)", "rot_deg", -180.0, 180.0, 0.0)
        self._fxrow_labeled(xf, 2, "X offset (px)", "off_x",  -120.0, 120.0, 0.0)
        self._fxrow_labeled(xf, 3, "Y offset (px)", "off_y",  -120.0, 120.0, 0.0)
        ttk.Label(xf, text="Positive X → right,  positive Y → down,  rotation CW.",
                  foreground="#888").grid(row=4, column=0, columnspan=3, sticky="w", padx=4, pady=(2, 4))

    def _params(self):
        p = {k: v.get() for k, v in self.fx.items()}
        p["tint_color"] = self.tint_color
        p["invert"] = self.invert_var.get()
        return p

    def _pick_tint(self):
        c = colorchooser.askcolor(color=self.tint_color, title="Tint color")
        if c and c[0]:
            self.tint_color = tuple(int(x) for x in c[0])
            self._refresh_preview()

    def _reset_fx(self):
        for k, v in self.fx.items():
            v.set(self._fx_def[k])
        self.tint_color = (255, 255, 255)
        self.invert_var.set(False)
        self._refresh_preview()

    # ---------- loading ----------
    def _open(self):
        p = filedialog.askopenfilename(
            filetypes=[("Animations", "*.gif *.webp *.png *.apng *.mp4 *.mov *.gifv *.mkv *.avi *.webm"),
                       ("All", "*.*")])
        if p:
            self._load(p)

    def _load(self, path):
        try:
            self.status.config(text=f"loading {os.path.basename(path)} …")
            self.root.update_idletasks()
            self.frames, self.durations = M.load_source(path)
        except Exception as e:
            messagebox.showerror("Load failed", str(e))
            return
        if not self.frames:
            messagebox.showerror("Load failed", "no frames")
            return
        w, h = self.frames[0].size
        self.cx, self.cy, self.s = w / 2, h / 2, min(w, h)   # start: full short side, centred
        self.cur = 0
        self.in_f, self.out_f = 0, len(self.frames) - 1
        self.scrub.config(to=len(self.frames) - 1)
        self.scrub.set(0)
        dur = sum(self.durations) / 1000.0
        self.info.config(text=f"{os.path.basename(path)}  |  {len(self.frames)} frames, "
                              f"{w}x{h}, {dur:.2f}s")
        self._redraw()
        self._draw_timeline()

    # ---------- crop interaction ----------
    def _fit(self):
        w, h = self.frames[self.cur].size
        sc = min(SRC_CANVAS / w, SRC_CANVAS / h)
        ox = (SRC_CANVAS - w * sc) / 2
        oy = (SRC_CANVAS - h * sc) / 2
        return sc, ox, oy

    def _press(self, e):
        sc, ox, oy = self._fit()
        self._drag = ((e.x - ox) / sc, (e.y - oy) / sc)

    def _drag_move(self, e):
        if not self._drag or not self.frames:
            return
        sc, ox, oy = self._fit()
        sx, sy = (e.x - ox) / sc, (e.y - oy) / sc
        self.cx += sx - self._drag[0]
        self.cy += sy - self._drag[1]
        self._drag = (sx, sy)
        self._redraw()

    def _wheel(self, e):
        self._zoom(1.1 if e.delta > 0 else 0.9)

    def _zoom(self, f):
        if not self.frames:
            return
        self.s = max(16.0, self.s / f)   # wheel up => zoom in => smaller crop
        self._redraw()

    # ---------- rendering ----------
    def _process(self, im, t=0.0):
        half = self.s / 2.0
        box = (round(self.cx - half), round(self.cy - half),
               round(self.cx + half), round(self.cy + half))
        out = im.crop(box).resize((M.W, M.H), Image.LANCZOS)
        out = FX.process(out, self._params(), t)
        # Apply baked-in transform (offset then rotate) before circle mask so
        # rotated corners are cleanly cropped by the circle, not left as artifacts.
        ox = int(round(self.fx["off_x"].get()))
        oy = int(round(self.fx["off_y"].get()))
        if ox != 0 or oy != 0:
            bg = Image.new("RGB", (M.W, M.H), (0, 0, 0))
            bg.paste(out, (ox, oy))
            out = bg
        deg = self.fx["rot_deg"].get()
        if deg != 0.0:
            out = out.rotate(-deg, resample=Image.BICUBIC, expand=False)
        if self.mask.get():
            out = M.apply_circle(out)
        return out

    def _redraw(self):
        if not self.frames:
            return
        im = self.frames[self.cur]
        sc, ox, oy = self._fit()
        disp = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.BILINEAR)
        self.src_photo = ImageTk.PhotoImage(disp)
        self.src.delete("all")
        self.src.create_image(ox, oy, anchor="nw", image=self.src_photo)
        # crop rect + circle in canvas coords
        x0 = ox + (self.cx - self.s / 2) * sc
        y0 = oy + (self.cy - self.s / 2) * sc
        x1 = ox + (self.cx + self.s / 2) * sc
        y1 = oy + (self.cy + self.s / 2) * sc
        self.src.create_rectangle(x0, y0, x1, y1, outline="#21d4fd", width=2)
        self.src.create_oval(x0, y0, x1, y1, outline="#ff3b3b", width=2)
        self._refresh_preview()

    def _preview_t(self):
        span = max(1, self.out_f - self.in_f)
        return min(1.0, max(0.0, (self.cur - self.in_f) / span))

    def _refresh_preview(self):
        if not self.frames:
            return
        out = self._process(self.frames[self.cur], self._preview_t())
        out = out.resize((PREV_CANVAS, PREV_CANVAS), Image.NEAREST)
        self.prev_photo = ImageTk.PhotoImage(out)
        self.prev.delete("all")
        self.prev.create_image(0, 0, anchor="nw", image=self.prev_photo)

    def _on_scrub(self, _v):
        if self.frames:
            self.cur = int(float(self.scrub.get()))
            self._redraw()
            self._draw_timeline()

    # ---------- trim ----------
    def _set_in(self):
        if self.frames:
            self.in_f = min(self.cur, self.out_f)
            self._draw_timeline()

    def _set_out(self):
        if self.frames:
            self.out_f = max(self.cur, self.in_f)
            self._draw_timeline()

    def _reset_trim(self):
        if self.frames:
            self.in_f, self.out_f = 0, len(self.frames) - 1
            self._draw_timeline()

    def _draw_timeline(self):
        if not self.frames:
            return
        self.tl.update_idletasks()
        w = self.tl.winfo_width() or 600
        h = int(self.tl["height"])
        n = len(self.frames)
        span = max(1, n - 1)
        x0 = self.in_f / span * w
        x1 = self.out_f / span * w
        xc = self.cur / span * w
        self.tl.delete("all")
        self.tl.create_rectangle(0, 0, w, h, fill="#333", outline="")
        self.tl.create_rectangle(x0, 0, x1, h, fill="#2b8a3e", outline="")
        self.tl.create_line(xc, 0, xc, h, fill="#fff", width=2)
        kept = self.out_f - self.in_f + 1
        secs = sum(self.durations[self.in_f:self.out_f + 1]) / 1000.0
        self.trim_lbl.config(text=f"in {self.in_f}   out {self.out_f}   "
                                  f"({kept} frames, {secs:.2f}s)")

    def _tl_click(self, e):
        if not self.frames:
            return
        w = self.tl.winfo_width() or 600
        n = len(self.frames)
        f = int(round(e.x / max(1, w) * (n - 1)))
        self.cur = max(0, min(n - 1, f))
        self.scrub.set(self.cur)
        self._redraw()
        self._draw_timeline()

    # ---------- export ----------
    def _browse_out(self):
        p = filedialog.asksaveasfilename(defaultextension=".eyv",
                                         filetypes=[("eye clip", "*.eyv")])
        if p:
            self.outpath.set(p)

    def _export(self):
        if not self.frames:
            return
        fmt = M.FMT_RAW if self.fmt.get() == "raw" else M.FMT_MJPEG
        fps = max(1, self.fps.get())
        lo, hi = self.in_f, self.out_f + 1            # trim to the selected range
        frames = M.resample(self.frames[lo:hi], self.durations[lo:hi], fps, 0)
        n = max(1, len(frames))
        self.status.config(text=f"encoding {n} frames …")
        self.root.update_idletasks()
        # t = i/n so animated effects loop seamlessly back to the start
        blobs = [M.frame_to_blob(self._process(im, i / n), fmt, self.quality.get())
                 for i, im in enumerate(frames)]
        total = M.pack_eyv(self.outpath.get(), blobs, fps, fmt, True)
        warn = "  !! > 10MB partition" if total > 10 * 1024 * 1024 else ""
        self.status.config(
            text=f"wrote {os.path.basename(self.outpath.get())}: {len(blobs)} frames, "
                 f"{total/1024:.0f} KiB ({total/len(blobs):.0f} B/frame){warn}   "
                 f"->  .\\tools\\flash_clip.ps1 -Port COMx")


def main():
    root = tk.Tk()
    EyeStudio(root, sys.argv[1] if len(sys.argv) > 1 else None)
    root.mainloop()


if __name__ == "__main__":
    main()
