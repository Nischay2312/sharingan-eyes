#!/usr/bin/env python3
"""
Sharingan Eye – Clip Manager
Talks to the board's existing HTTP REST API over WiFi.
Requires: pip install requests
"""

import os
import sys
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

try:
    import requests
except ImportError:
    sys.exit("Missing dependency — run:  pip install requests")


# ── tunables ──────────────────────────────────────────────────────────────────
UPLOAD_CHUNK   = 8192          # bytes per chunk sent to the board
UPLOAD_TIMEOUT = 180           # seconds before giving up on upload
CONNECT_TIMEOUT = 6            # seconds for status/delete calls
# ─────────────────────────────────────────────────────────────────────────────


class EyeManager(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Sharingan Eye – Clip Manager")
        self.geometry("660x500")
        self.resizable(True, True)
        self.configure(bg="#111")

        self._host  = tk.StringVar(value="sharingan-eye.local")
        self._clips = []          # list of clip name strings
        self._busy  = False

        self._build_ui()
        self.after(200, self._refresh)   # auto-connect on launch

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self):
        DARK, MID, LIGHT, RED, GREEN = "#111", "#1a1a26", "#ddd", "#e0141b", "#1a6b2a"

        # ── connection bar ──
        top = tk.Frame(self, bg=DARK, pady=8, padx=10)
        top.pack(fill=tk.X)

        tk.Label(top, text="Board:", bg=DARK, fg=LIGHT).pack(side=tk.LEFT)
        tk.Entry(top, textvariable=self._host, width=28,
                 bg=MID, fg=LIGHT, insertbackground=LIGHT,
                 relief=tk.FLAT, bd=4).pack(side=tk.LEFT, padx=6)
        tk.Button(top, text="Connect / Refresh", command=self._refresh,
                  bg="#24242f", fg=LIGHT, relief=tk.FLAT,
                  activebackground="#333", padx=8).pack(side=tk.LEFT)

        # ── storage bar ──
        self._storage_var = tk.StringVar(value="—")
        tk.Label(self, textvariable=self._storage_var,
                 bg=DARK, fg="#888", anchor=tk.W,
                 font=("Courier", 11)).pack(fill=tk.X, padx=12, pady=(4, 0))

        # ── clip list ──
        list_frame = tk.Frame(self, bg=DARK, padx=10, pady=4)
        list_frame.pack(fill=tk.BOTH, expand=True)

        cols = ("name", "size_kb")
        self._tree = ttk.Treeview(list_frame, columns=cols,
                                   show="headings", selectmode="extended")
        self._tree.heading("name",    text="Clip filename")
        self._tree.heading("size_kb", text="Size (KB)")
        self._tree.column("name",    width=460, anchor=tk.W)
        self._tree.column("size_kb", width=90,  anchor=tk.CENTER)

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Treeview",
                         background=MID, foreground=LIGHT,
                         fieldbackground=MID, rowheight=26)
        style.configure("Treeview.Heading",
                         background="#24242f", foreground="#888")
        style.map("Treeview", background=[("selected", "#2a2a40")])

        sb = ttk.Scrollbar(list_frame, orient=tk.VERTICAL,
                           command=self._tree.yview)
        self._tree.configure(yscrollcommand=sb.set)
        self._tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.pack(side=tk.RIGHT, fill=tk.Y)

        # double-click → play on board
        self._tree.bind("<Double-Button-1>", self._on_double_click)

        # ── action buttons ──
        btn_frame = tk.Frame(self, bg=DARK, padx=10, pady=6)
        btn_frame.pack(fill=tk.X)

        def btn(parent, text, cmd, bg="#24242f"):
            return tk.Button(parent, text=text, command=cmd,
                             bg=bg, fg=LIGHT, relief=tk.FLAT,
                             activebackground="#333", padx=10, pady=6)

        btn(btn_frame, "⬆  Upload .eyv",    self._upload_dialog,  bg=GREEN).pack(side=tk.LEFT, padx=4)
        btn(btn_frame, "✕  Delete selected", self._delete_selected, bg="#6b1a1a").pack(side=tk.LEFT, padx=4)
        btn(btn_frame, "▶  Play selected",   self._play_selected).pack(side=tk.LEFT, padx=4)
        btn(btn_frame, "↺  Refresh",         self._refresh).pack(side=tk.RIGHT, padx=4)

        # ── progress ──
        self._progress = ttk.Progressbar(self, mode="determinate",
                                          style="green.Horizontal.TProgressbar")
        style.configure("green.Horizontal.TProgressbar",
                         troughcolor="#1a1a26", background="#1a6b2a")
        self._progress.pack(fill=tk.X, padx=10, pady=2)

        # ── status ──
        self._status = tk.StringVar(value="Connecting…")
        tk.Label(self, textvariable=self._status,
                 bg=DARK, fg="#888", anchor=tk.W,
                 font=("Courier", 11)).pack(fill=tk.X, padx=12, pady=(2, 6))

    # ── helpers ───────────────────────────────────────────────────────────────

    def _base(self):
        h = self._host.get().strip()
        return h if h.startswith("http") else f"http://{h}"

    def _set_status(self, msg, color="#888"):
        self._status.set(msg)
        # find the status label and change its colour
        for w in self.winfo_children():
            if isinstance(w, tk.Label) and w.cget("textvariable") == str(self._status):
                w.config(fg=color)
                break

    def _set_busy(self, busy):
        self._busy = busy
        state = tk.DISABLED if busy else tk.NORMAL
        for w in self.winfo_children():
            if isinstance(w, tk.Frame):
                for b in w.winfo_children():
                    if isinstance(b, tk.Button):
                        try:
                            b.config(state=state)
                        except Exception:
                            pass

    # ── connect / refresh ─────────────────────────────────────────────────────

    def _refresh(self):
        if self._busy:
            return
        self._set_status("Connecting…")
        threading.Thread(target=self._refresh_worker, daemon=True).start()

    def _refresh_worker(self):
        try:
            r = requests.get(f"{self._base()}/api/status", timeout=CONNECT_TIMEOUT)
            r.raise_for_status()
            data = r.json()
        except requests.exceptions.ConnectionError:
            self.after(0, lambda: self._set_status(
                f"Cannot reach {self._host.get()} — check IP/hostname and WiFi", "#e0141b"))
            return
        except Exception as e:
            self.after(0, lambda: self._set_status(f"Error: {e}", "#e0141b"))
            return

        self.after(0, lambda: self._apply_status(data))

    def _apply_status(self, data):
        stor = data.get("storage") or {}
        free_mb  = stor.get("kb_free",  0) / 1024
        total_mb = stor.get("kb_total", 0) / 1024
        used_mb  = total_mb - free_mb
        bar_w    = 40
        filled   = int(bar_w * used_mb / total_mb) if total_mb else 0
        bar      = "█" * filled + "░" * (bar_w - filled)
        self._storage_var.set(
            f"  [{bar}]  {free_mb:.1f} MB free of {total_mb:.1f} MB"
            if total_mb else "  Storage info unavailable")

        self._clips = data.get("clips") or []
        self._tree.delete(*self._tree.get_children())
        for c in self._clips:
            self._tree.insert("", tk.END, iid=c, values=(c, ""))

        board = data.get("board") or data.get("id") or self._host.get()
        self._set_status(
            f"Connected — {board}  ·  {len(self._clips)} clip(s)", "#888")

    # ── upload ────────────────────────────────────────────────────────────────

    def _upload_dialog(self):
        if self._busy:
            return
        paths = filedialog.askopenfilenames(
            title="Select .eyv clip(s) to upload",
            filetypes=[("Eye animation", "*.eyv"), ("All files", "*.*")])
        if not paths:
            return
        non_eyv = [p for p in paths if not p.lower().endswith(".eyv")]
        if non_eyv:
            messagebox.showwarning("Wrong file type",
                                   "Only .eyv files are supported:\n" +
                                   "\n".join(os.path.basename(p) for p in non_eyv))
            return
        self._set_busy(True)
        threading.Thread(target=self._upload_worker, args=(list(paths),), daemon=True).start()

    def _upload_worker(self, paths):
        for i, path in enumerate(paths):
            name = os.path.basename(path)
            size = os.path.getsize(path)
            label = f"Uploading {name}  ({size/1024:.0f} KB)"
            if len(paths) > 1:
                label += f"  [{i+1}/{len(paths)}]"
            self.after(0, lambda l=label: [
                self._set_status(l),
                self._progress.config(value=0)])

            uploaded = [0]

            def gen(p, s):
                with open(p, "rb") as f:
                    while True:
                        chunk = f.read(UPLOAD_CHUNK)
                        if not chunk:
                            break
                        uploaded[0] += len(chunk)
                        pct = int(uploaded[0] * 100 / s)
                        self.after(0, lambda v=pct: self._progress.config(value=v))
                        yield chunk

            try:
                url = f"{self._base()}/api/upload?name={requests.utils.quote(name)}"
                resp = requests.post(
                    url, data=gen(path, size),
                    headers={"Content-Length": str(size),
                             "Content-Type": "application/octet-stream"},
                    timeout=UPLOAD_TIMEOUT)
                resp.raise_for_status()
                self.after(0, lambda n=name: self._set_status(f"✓ Uploaded {n}", "#4caf50"))
            except requests.exceptions.HTTPError as e:
                msg = e.response.text if e.response else str(e)
                self.after(0, lambda m=msg, n=name: [
                    messagebox.showerror("Upload failed", f"{n}:\n{m}"),
                    self._set_status(f"Upload failed — {m}", "#e0141b")])
                break
            except Exception as e:
                self.after(0, lambda e=e, n=name: [
                    messagebox.showerror("Upload failed", f"{n}:\n{e}"),
                    self._set_status(f"Upload failed — {e}", "#e0141b")])
                break

        self.after(0, lambda: [
            self._progress.config(value=0),
            self._set_busy(False),
            self._refresh()])

    # ── delete ────────────────────────────────────────────────────────────────

    def _delete_selected(self):
        if self._busy:
            return
        sel = self._tree.selection()
        if not sel:
            messagebox.showinfo("Nothing selected", "Select one or more clips first.")
            return
        names = [self._tree.item(s)["values"][0] for s in sel]
        plural = "s" if len(names) > 1 else ""
        if not messagebox.askyesno(
                f"Delete {len(names)} clip{plural}?",
                "This cannot be undone:\n\n" + "\n".join(names)):
            return
        self._set_busy(True)
        threading.Thread(target=self._delete_worker, args=(names,), daemon=True).start()

    def _delete_worker(self, names):
        for name in names:
            self.after(0, lambda n=name: self._set_status(f"Deleting {n}…"))
            try:
                r = requests.post(f"{self._base()}/api/delete",
                                   json={"name": name},
                                   timeout=CONNECT_TIMEOUT)
                r.raise_for_status()
            except Exception as e:
                self.after(0, lambda e=e, n=name: [
                    messagebox.showerror("Delete failed", f"{n}:\n{e}"),
                    self._set_status(f"Delete failed — {e}", "#e0141b")])
                break
        self.after(0, lambda: [self._set_busy(False), self._refresh()])

    # ── play ──────────────────────────────────────────────────────────────────

    def _play_selected(self):
        sel = self._tree.selection()
        if not sel:
            return
        name = self._tree.item(sel[0])["values"][0]
        threading.Thread(target=self._play_worker, args=(name,), daemon=True).start()

    def _play_worker(self, name):
        try:
            requests.post(f"{self._base()}/api/clip",
                           json={"name": name},
                           timeout=CONNECT_TIMEOUT)
            self.after(0, lambda: self._set_status(f"▶ Playing {name}"))
        except Exception as e:
            self.after(0, lambda: self._set_status(f"Play failed — {e}", "#e0141b"))

    # ── double-click = play ───────────────────────────────────────────────────

    def _on_double_click(self, _event):
        self._play_selected()


# ── entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = EyeManager()
    app.mainloop()
