#!/usr/bin/env python3
"""
make_pack.py -- bundle several .eyv clips into one .eyp pack for the flash board.

The LCD board has a single 'anim' flash partition (no folder), so to get a
playlist there you pack multiple clips into one file and flash that. The AMOLED
board doesn't need this -- just drop multiple .eyv files in the SD folder.

  python make_pack.py intro.eyv loop.eyv outro.eyv -o pack.eyp
  .\tools\flash_clip.ps1 -Port COM5 -Clip pack.eyp     # flashes to 'anim' @ 0x410000

.eyp layout (little-endian):
  Header 16B: "EYEP", version u16, clip_count u16, reserved[8]
  Directory:  clip_count * { offset u32, size u32 }   (offset from pack start)
  Data:       concatenated .eyv files (each a complete clip)
"""

import argparse
import struct
import sys

MAGIC = b"EYEP"
VERSION = 1
MAX_CLIPS = 24


def main():
    ap = argparse.ArgumentParser(description="Bundle .eyv clips into one .eyp pack")
    ap.add_argument("clips", nargs="+", help="input .eyv files, in play order")
    ap.add_argument("-o", "--output", default="pack.eyp")
    args = ap.parse_args()

    if len(args.clips) > MAX_CLIPS:
        sys.exit(f"too many clips ({len(args.clips)}); firmware plays at most {MAX_CLIPS}")

    blobs = []
    for path in args.clips:
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] != b"EYEV":
            sys.exit(f"{path} is not a .eyv clip (bad magic)")
        blobs.append(data)

    header = MAGIC + struct.pack("<HH8x", VERSION, len(blobs))
    data_start = len(header) + len(blobs) * 8
    offsets, cur = [], data_start
    for b in blobs:
        offsets.append(cur)
        cur += len(b)
    directory = b"".join(struct.pack("<II", o, len(b)) for o, b in zip(offsets, blobs))

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(directory)
        for b in blobs:
            f.write(b)

    print(f"wrote {args.output}: {len(blobs)} clips, {cur/1024:.0f} KiB total")
    for i, (path, b) in enumerate(zip(args.clips, blobs)):
        print(f"  {i+1}. {path}  ({len(b)/1024:.0f} KiB)")
    if cur > 10 * 1024 * 1024:
        print("WARNING: larger than the 10 MB 'anim' partition.")
    print(f"flash with:  .\\tools\\flash_clip.ps1 -Port COMx -Clip {args.output}")


if __name__ == "__main__":
    main()
