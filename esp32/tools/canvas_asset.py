#!/usr/bin/env python3
"""Prepare a device-compatible RGB565 or RGB baseline JPEG, with a PNG preview."""
import argparse
from pathlib import Path


def main():
    from PIL import Image, ImageDraw, ImageFont, ImageOps

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path)
    parser.add_argument("destination", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--text")
    parser.add_argument("--font", type=Path)
    parser.add_argument("--font-size", type=int, default=32)
    args = parser.parse_args()
    output = args.output or args.destination
    if not output or not (1 <= args.width <= 480 and 1 <= args.height <= 800):
        parser.error("provide an output and dimensions within 480x800")
    if output.suffix.lower() not in (".rgb565", ".raw", ".jpg", ".jpeg"):
        parser.error("output must end in .rgb565, .raw, .jpg or .jpeg")
    if args.input and output.resolve() == args.input.resolve():
        parser.error("output must differ from the input image")
    if (args.input is None) == (args.text is None):
        parser.error("provide either an input image or --text")
    canvas = Image.new("RGB", (args.width, args.height), "white")
    if args.input:
        with Image.open(args.input) as source:
            picture = ImageOps.contain(ImageOps.exif_transpose(source).convert("RGBA"), canvas.size)
            canvas.paste(picture, ((args.width - picture.width) // 2,
                                  (args.height - picture.height) // 2), picture)
    else:
        if not args.font or not (1 <= args.font_size <= 800):
            parser.error("--text requires --font and a positive --font-size <= 800")
        font = ImageFont.truetype(str(args.font), args.font_size)
        draw = ImageDraw.Draw(canvas)
        line = ""
        lines = []
        for char in args.text:
            if char == "\n":
                lines.append(line)
                line = ""
            elif line and draw.textlength(line + char, font=font) > args.width:
                lines.append(line)
                line = char
            else:
                line += char
        lines.append(line)
        draw.multiline_text((0, 0), "\n".join(lines), font=font, fill="black", spacing=4)
    if output.suffix.lower() in (".jpg", ".jpeg"):
        canvas.save(output, format="JPEG", quality=90, progressive=False, subsampling=2)
    else:
        raw = bytearray()
        rgb = canvas.tobytes()
        for i in range(0, len(rgb), 3):
            r, g, b = rgb[i:i + 3]
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            raw.extend((value >> 8, value & 255))
        output.write_bytes(raw)
    preview = output.with_name(output.name + ".png")
    canvas.save(preview)
    print(f"{output}: {args.width}x{args.height}, {output.stat().st_size} bytes; preview {preview}")


if __name__ == "__main__":
    main()
