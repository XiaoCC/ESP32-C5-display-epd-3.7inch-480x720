"""Build the 16 px quote font from the OFL Noto Sans CJK SC Bold OTF."""

import argparse
import struct

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("font")
parser.add_argument("output")
args = parser.parse_args()

ranges = [(0x20, 0x024F), (0x0370, 0x052F), (0x2000, 0x206F),
          (0x3000, 0x30FF), (0x3400, 0x9FFF), (0xFF00, 0xFFEF)]
with TTFont(args.font) as source:
    available = sorted(source.getBestCmap())
face = ImageFont.truetype(args.font, 15)
image = Image.new("1", (16, 16))
draw = ImageDraw.Draw(image)
with open(args.output, "wb") as output:
    for code in available:
        if not any(start <= code <= end for start, end in ranges):
            continue
        draw.rectangle((0, 0, 15, 15), fill=0)
        draw.text((0, 13), chr(code), font=face, fill=1, anchor="ls")
        rows = (int.from_bytes(image.tobytes()[offset:offset + 2], "big")
                for offset in range(0, 32, 2))
        output.write(struct.pack("<H16H", code, *rows))
