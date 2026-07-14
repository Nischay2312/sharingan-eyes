#!/usr/bin/env python3
"""
make_anim.py -- convert a GIF/video into a 240x240 .eyv clip for the eye board.

  python make_anim.py input.gif -o anim.eyv --fps 20 --quality 80

The .eyv container (little-endian) is:
  Header 32B: "EYEV", version u16, format u16(0=MJPEG,1=raw), w u16, h u16,
              frame_count u32, frame_ms u16, loop u8, reserved[13]
  Index: frame_count * { offset u32, size u32 }   (offset from file start)
  Data:  concatenated JPEG (or raw RGB565_LE) frame blobs

Flash it to the board's 'anim' partition (no app rebuild needed):
  parttool.py --port COM4 --partition-name anim write_partition --input anim.eyv
"""

import argparse
import struct
import sys

import numpy as np
from PIL import Image, ImageEnhance

W = H = 240
MAGIC = b"EYEV"
VERSION = 1
FMT_MJPEG = 0
FMT_RAW = 1


def load_source(path):
    """Return (list[PIL.Image RGB], list[duration_ms])."""
    lower = path.lower()
    if lower.endswith((".gif", ".webp", ".apng", ".png")):
        im = Image.open(path)
        frames, durations = [], []
        try:
            i = 0
            while True:
                im.seek(i)
                # Composite onto black so transparency doesn't become garbage.
                rgba = im.convert("RGBA")
                bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
                bg.alpha_composite(rgba)
                frames.append(bg.convert("RGB"))
                durations.append(im.info.get("duration", 100) or 100)
                i += 1
        except EOFError:
            pass
        if frames:
            return frames, durations
        # single image
        return [Image.open(path).convert("RGB")], [100]

    # video -> needs imageio + imageio-ffmpeg
    try:
        import imageio.v2 as imageio
    except ImportError:
        sys.exit("Reading video needs imageio: pip install imageio imageio-ffmpeg")
    rdr = imageio.get_reader(path)
    fps = rdr.get_meta_data().get("fps", 25) or 25
    dur = 1000.0 / fps
    frames, durations = [], []
    for fr in rdr:
        arr = np.asarray(fr)
        if arr.ndim == 2:
            arr = np.stack([arr] * 3, -1)
        frames.append(Image.fromarray(arr[:, :, :3], "RGB"))
        durations.append(dur)
    rdr.close()
    return frames, durations


def resample(frames, durations, fps, max_frames):
    """Resample a variable-duration timeline to a fixed fps."""
    total = sum(durations)
    if total <= 0 or len(frames) <= 1:
        return frames[:max_frames] if max_frames else frames
    ends, acc = [], 0.0
    for d in durations:
        acc += d
        ends.append(acc)
    step = 1000.0 / fps
    out, k = [], 0
    while True:
        t = k * step
        if t >= total:
            break
        idx = next((j for j, e in enumerate(ends) if e > t), len(frames) - 1)
        out.append(frames[idx])
        k += 1
        if max_frames and len(out) >= max_frames:
            break
    return out or frames[:1]


def fit_square(im, mode, zoom=1.0):
    if mode == "cover":
        w, h = im.size
        s = max(8, int(min(w, h) / max(0.1, zoom)))   # zoom>1 crops a smaller centre
        im = im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
        return im.resize((W, H), Image.LANCZOS)
    im = im.copy()
    im.thumbnail((W, H), Image.LANCZOS)
    bg = Image.new("RGB", (W, H), (0, 0, 0))
    bg.paste(im, ((W - im.width) // 2, (H - im.height) // 2))
    return bg


_CIRCLE = None


def apply_circle(im):
    global _CIRCLE
    if _CIRCLE is None:
        yy, xx = np.ogrid[:H, :W]
        c = (W - 1) / 2.0
        _CIRCLE = (xx - c) ** 2 + (yy - c) ** 2 > (W / 2.0) ** 2
    arr = np.array(im)
    arr[_CIRCLE] = 0
    return Image.fromarray(arr, "RGB")


def color_adjust(im, sat, gamma):
    if sat != 1.0:
        im = ImageEnhance.Color(im).enhance(sat)
    if gamma != 1.0:
        lut = [min(255, int(255 * ((i / 255.0) ** (1.0 / gamma)))) for i in range(256)]
        im = im.point(lut * 3)
    return im


def to_rgb565_le(im):
    a = np.asarray(im).astype(np.uint16)
    v = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
    return v.astype("<u2").tobytes()


def encode_jpeg(im, quality):
    import io
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality, optimize=True)
    return buf.getvalue()


def frame_to_blob(im240, fmt, quality):
    """Encode a finished 240x240 RGB image to a clip frame blob."""
    return to_rgb565_le(im240) if fmt == FMT_RAW else encode_jpeg(im240, quality)


def pack_eyv(out_path, blobs, fps, fmt, loop):
    """Write the .eyv container. Returns total byte size."""
    frame_ms = max(1, round(1000.0 / fps))
    header = MAGIC + struct.pack("<HHHHIHB13x", VERSION, fmt, W, H,
                                 len(blobs), frame_ms, 1 if loop else 0)
    data_start = len(header) + len(blobs) * 8
    offsets, cur = [], data_start
    for b in blobs:
        offsets.append(cur)
        cur += len(b)
    index = b"".join(struct.pack("<II", o, len(b)) for o, b in zip(offsets, blobs))
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(index)
        for b in blobs:
            f.write(b)
    return cur


def apply_transform(im, rotate_deg=0.0, off_x=0, off_y=0):
    """Shift then rotate a 240x240 frame; corners filled with black."""
    if off_x != 0 or off_y != 0:
        bg = Image.new("RGB", (W, H), (0, 0, 0))
        bg.paste(im, (off_x, off_y))
        im = bg
    if rotate_deg != 0.0:
        im = im.rotate(-rotate_deg, resample=Image.BICUBIC, expand=False)
    return im


def main():
    ap = argparse.ArgumentParser(description="Build a 240x240 .eyv clip for the eye board")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", default="anim.eyv")
    ap.add_argument("--fps", type=int, default=20)
    ap.add_argument("--quality", type=int, default=80, help="JPEG quality (mjpeg)")
    ap.add_argument("--format", choices=["mjpeg", "raw"], default="mjpeg")
    ap.add_argument("--fit", choices=["cover", "contain"], default="cover")
    ap.add_argument("--zoom", type=float, default=1.0,
                    help="cover zoom; >1 crops tighter (e.g. 1.8 to fill with the iris)")
    ap.add_argument("--mask", choices=["circle", "none"], default="circle")
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--no-loop", action="store_true")
    ap.add_argument("--saturation", type=float, default=1.0)
    ap.add_argument("--gamma", type=float, default=1.0)
    ap.add_argument("--rotate", type=float, default=0.0,
                    help="rotate content CW by this many degrees (baked in, 0=none)")
    ap.add_argument("--offset-x", type=int, default=0,
                    help="shift content right by N pixels (positive=right)")
    ap.add_argument("--offset-y", type=int, default=0,
                    help="shift content down by N pixels (positive=down)")
    args = ap.parse_args()

    print(f"reading {args.input} ...")
    frames, durations = load_source(args.input)
    print(f"  {len(frames)} source frames, {sum(durations)/1000:.2f}s")
    frames = resample(frames, durations, args.fps, args.max_frames)
    print(f"  -> {len(frames)} frames @ {args.fps} fps")

    fmt = FMT_RAW if args.format == "raw" else FMT_MJPEG
    blobs = []
    for im in frames:
        im = fit_square(im, args.fit, args.zoom)
        im = color_adjust(im, args.saturation, args.gamma)
        im = apply_transform(im, args.rotate, args.offset_x, args.offset_y)
        if args.mask == "circle":
            im = apply_circle(im)
        blobs.append(frame_to_blob(im, fmt, args.quality))

    total = pack_eyv(args.output, blobs, args.fps, fmt, not args.no_loop)
    avg = total / max(1, len(blobs))
    print(f"wrote {args.output}: {len(blobs)} frames, {total/1024:.0f} KiB "
          f"({avg:.0f} B/frame avg), {args.format}, loop={'no' if args.no_loop else 'yes'}")
    if total > 10 * 1024 * 1024:
        print("WARNING: larger than the 10 MB 'anim' partition -- lower --fps/--quality "
              "or --max-frames.")
    print("flash with:\n  parttool.py --partition-name anim write_partition "
          f"--input {args.output}")


if __name__ == "__main__":
    main()
