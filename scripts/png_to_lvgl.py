#!/usr/bin/env python3
"""Convert a PNG into an LVGL v8 true-colour-with-alpha C image descriptor.

Stdlib only: the toolchain Python shipped with MSYS2/PlatformIO has no Pillow.
Only what this project needs is supported: non-interlaced 8-bit PNGs and the
RGB565 (LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 0) target format.

Usage:
  python scripts/png_to_lvgl.py INPUT.png OUTPUT.c SYMBOL --height PX
"""

import argparse
import pathlib
import struct
import sys
import zlib

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def read_chunks(blob):
    if blob[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG file")
    offset = 8
    while offset < len(blob):
        (length,) = struct.unpack(">I", blob[offset:offset + 4])
        kind = blob[offset + 4:offset + 8]
        data = blob[offset + 8:offset + 8 + length]
        yield kind, data
        offset += length + 12


def unfilter(raw, width, height, bpp):
    stride = width * bpp
    out = bytearray(stride * height)
    previous = bytearray(stride)
    pos = 0
    for row in range(height):
        method = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if method == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif method == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif method == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif method == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                up = previous[i]
                upleft = previous[i - bpp] if i >= bpp else 0
                estimate = left + up - upleft
                da, db, dc = (abs(estimate - left), abs(estimate - up),
                              abs(estimate - upleft))
                nearest = left if (da <= db and da <= dc) else (
                    up if db <= dc else upleft)
                line[i] = (line[i] + nearest) & 0xFF
        elif method != 0:
            raise SystemExit("unsupported PNG row filter %d" % method)
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return out


def decode_png(path):
    """Return (width, height, RGBA bytes)."""
    blob = path.read_bytes()
    width = height = depth = color_type = None
    palette = b""
    trns = b""
    idat = bytearray()
    for kind, data in read_chunks(blob):
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", data)
            if depth != 8:
                raise SystemExit("only 8-bit PNGs are supported")
            if interlace:
                raise SystemExit("interlaced PNGs are not supported")
            if color_type not in CHANNELS:
                raise SystemExit("unsupported colour type %d" % color_type)
        elif kind == b"PLTE":
            palette = data
        elif kind == b"tRNS":
            trns = data
        elif kind == b"IDAT":
            idat += data
        elif kind == b"IEND":
            break

    bpp = CHANNELS[color_type]
    rows = unfilter(zlib.decompress(bytes(idat)), width, height, bpp)
    rgba = bytearray(width * height * 4)
    for index in range(width * height):
        source = rows[index * bpp:index * bpp + bpp]
        if color_type == 0:
            pixel = (source[0], source[0], source[0], 255)
        elif color_type == 2:
            pixel = (source[0], source[1], source[2], 255)
        elif color_type == 3:
            entry = source[0]
            alpha = trns[entry] if entry < len(trns) else 255
            pixel = (palette[entry * 3], palette[entry * 3 + 1],
                     palette[entry * 3 + 2], alpha)
        elif color_type == 4:
            pixel = (source[0], source[0], source[0], source[1])
        else:
            pixel = (source[0], source[1], source[2], source[3])
        rgba[index * 4:index * 4 + 4] = bytes(pixel)
    return width, height, rgba


def trim(width, height, rgba):
    """Drop fully transparent borders so the logo scales to its ink bounds."""
    left, top, right, bottom = width, height, -1, -1
    for y in range(height):
        for x in range(width):
            if rgba[(y * width + x) * 4 + 3]:
                left, right = min(left, x), max(right, x)
                top, bottom = min(top, y), max(bottom, y)
    if right < 0:
        return width, height, rgba
    new_w, new_h = right - left + 1, bottom - top + 1
    out = bytearray(new_w * new_h * 4)
    for y in range(new_h):
        src = ((y + top) * width + left) * 4
        out[y * new_w * 4:(y + 1) * new_w * 4] = rgba[src:src + new_w * 4]
    return new_w, new_h, out


def resize(width, height, rgba, new_w, new_h):
    """Box filter with alpha-weighted colour so edges keep their hue."""
    out = bytearray(new_w * new_h * 4)
    for y in range(new_h):
        y0, y1 = y * height // new_h, max(y * height // new_h + 1,
                                          (y + 1) * height // new_h)
        for x in range(new_w):
            x0, x1 = x * width // new_w, max(x * width // new_w + 1,
                                             (x + 1) * width // new_w)
            alpha_sum = red = green = blue = 0
            count = 0
            for sy in range(y0, y1):
                for sx in range(x0, x1):
                    base = (sy * width + sx) * 4
                    alpha = rgba[base + 3]
                    alpha_sum += alpha
                    red += rgba[base] * alpha
                    green += rgba[base + 1] * alpha
                    blue += rgba[base + 2] * alpha
                    count += 1
            base = (y * new_w + x) * 4
            if alpha_sum:
                out[base] = red // alpha_sum
                out[base + 1] = green // alpha_sum
                out[base + 2] = blue // alpha_sum
                out[base + 3] = alpha_sum // count
    return out


def emit_c(symbol, width, height, rgba):
    """RGB565 little-endian pairs plus one alpha byte, as LVGL v8 expects."""
    body = []
    for y in range(height):
        chunk = []
        for x in range(width):
            base = (y * width + x) * 4
            red, green, blue, alpha = rgba[base:base + 4]
            packed = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
            chunk += ["0x%02x" % (packed & 0xFF), "0x%02x" % (packed >> 8),
                      "0x%02x" % alpha]
        for start in range(0, len(chunk), 12):
            body.append("    " + ", ".join(chunk[start:start + 12]) + ",")
    return """/* Generated by scripts/png_to_lvgl.py from assets/shunya_logo.png.
 * Do not edit by hand: re-run the script to change the size or artwork.
 * Format: LV_IMG_CF_TRUE_COLOR_ALPHA, RGB565 little-endian + alpha byte.
 */

#include "ui.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {symbol}_map[] = {{
{body}
}};

const lv_img_dsc_t {symbol} = {{
    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {width},
    .header.h = {height},
    .data_size = sizeof({symbol}_map),
    .data = {symbol}_map,
}};
""".format(symbol=symbol, body="\n".join(body), width=width, height=height)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("symbol")
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--max-width", type=int, default=0)
    args = parser.parse_args()

    width, height, rgba = decode_png(pathlib.Path(args.input))
    width, height, rgba = trim(width, height, rgba)
    new_h = args.height
    new_w = max(1, round(width * new_h / height))
    if args.max_width and new_w > args.max_width:
        new_w = args.max_width
        new_h = max(1, round(height * new_w / width))
    scaled = resize(width, height, rgba, new_w, new_h)

    text = emit_c(args.symbol, new_w, new_h, scaled)
    pathlib.Path(args.output).write_text(text, encoding="ascii")
    print("source %dx%d -> %dx%d, %d bytes of pixel data"
          % (width, height, new_w, new_h, new_w * new_h * 3), file=sys.stderr)


if __name__ == "__main__":
    main()
