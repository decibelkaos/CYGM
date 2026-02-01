#!/usr/bin/env python3
"""
Generate arrow image C arrays for CYGM project.
Creates const arrays stored in flash instead of RAM canvas buffers.
"""

from PIL import Image, ImageDraw
import struct

# Canvas size
WIDTH = 60
HEIGHT = 60

# Colors (RGB565 format)
def create_image():
    """Create a blank transparent RGBA image"""
    return Image.new('RGBA', (WIDTH, HEIGHT), (0, 0, 0, 0))

def draw_line_thick(draw, x1, y1, x2, y2, color_rgb, width):
    """Draw a thick line"""
    draw.line([(x1, y1), (x2, y2)], fill=color_rgb, width=width)

def draw_filled_triangle(draw, points, color_rgb):
    """Draw a filled triangle"""
    draw.polygon(points, fill=color_rgb)

def generate_flat_arrow():
    """Generate TREND_FLAT arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    green_rgba = (16, 185, 129, 255)
    blue_rgba = (59, 130, 246, 255)

    # Green glow shaft
    draw_line_thick(draw, 6, 30, 36, 30, green_rgba, 13)
    # Green arrowhead - larger triangle
    for i in range(20):
        draw.line([(36, 20+i), (54, 30)], fill=green_rgba, width=1)

    # Blue shaft
    draw_line_thick(draw, 6, 30, 36, 30, blue_rgba, 9)
    # Blue arrowhead - larger triangle
    for i in range(16):
        draw.line([(36, 22+i), (52, 30)], fill=blue_rgba, width=1)

    return img

def generate_45_up_arrow():
    """Generate TREND_FORTY_FIVE_UP arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    green_rgba = (16, 185, 129, 255)
    blue_rgba = (59, 130, 246, 255)

    # Green glow
    draw_line_thick(draw, 9, 51, 34, 26, green_rgba, 13)
    for i in range(16):
        draw.line([(34 + i//2, 26 - i//2), (52, 14)], fill=green_rgba, width=1)
        draw.line([(34 - i//2, 26 + i//2), (52, 14)], fill=green_rgba, width=1)

    # Blue arrow
    draw_line_thick(draw, 9, 51, 34, 26, blue_rgba, 9)
    for i in range(12):
        draw.line([(34 + i//2, 26 - i//2), (50, 16)], fill=blue_rgba, width=1)
        draw.line([(34 - i//2, 26 + i//2), (50, 16)], fill=blue_rgba, width=1)

    return img

def generate_45_down_arrow():
    """Generate TREND_FORTY_FIVE_DOWN arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    green_rgba = (16, 185, 129, 255)
    blue_rgba = (59, 130, 246, 255)

    # Green glow
    draw_line_thick(draw, 9, 9, 34, 34, green_rgba, 13)
    for i in range(16):
        draw.line([(34 + i//2, 34 + i//2), (52, 46)], fill=green_rgba, width=1)
        draw.line([(34 - i//2, 34 - i//2), (52, 46)], fill=green_rgba, width=1)

    # Blue arrow
    draw_line_thick(draw, 9, 9, 34, 34, blue_rgba, 9)
    for i in range(12):
        draw.line([(34 + i//2, 34 + i//2), (50, 44)], fill=blue_rgba, width=1)
        draw.line([(34 - i//2, 34 - i//2), (50, 44)], fill=blue_rgba, width=1)

    return img

def generate_single_up_arrow():
    """Generate TREND_SINGLE_UP arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    orange_rgba = (255, 165, 0, 255)
    blue_rgba = (59, 130, 246, 255)

    # Orange glow
    draw_line_thick(draw, 30, 51, 30, 20, orange_rgba, 13)
    draw_line_thick(draw, 30, 20, 20, 30, orange_rgba, 13)
    draw_line_thick(draw, 30, 20, 40, 30, orange_rgba, 13)

    # Blue arrow
    draw_line_thick(draw, 30, 51, 30, 20, blue_rgba, 9)
    draw_line_thick(draw, 30, 20, 20, 30, blue_rgba, 9)
    draw_line_thick(draw, 30, 20, 40, 30, blue_rgba, 9)

    return img

def generate_single_down_arrow():
    """Generate TREND_SINGLE_DOWN arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    orange_rgba = (255, 165, 0, 255)
    blue_rgba = (59, 130, 246, 255)

    # Orange glow
    draw_line_thick(draw, 30, 9, 30, 40, orange_rgba, 13)
    draw_line_thick(draw, 30, 40, 20, 30, orange_rgba, 13)
    draw_line_thick(draw, 30, 40, 40, 30, orange_rgba, 13)

    # Blue arrow
    draw_line_thick(draw, 30, 9, 30, 40, blue_rgba, 9)
    draw_line_thick(draw, 30, 40, 20, 30, blue_rgba, 9)
    draw_line_thick(draw, 30, 40, 40, 30, blue_rgba, 9)

    return img

def generate_double_up_arrow():
    """Generate TREND_DOUBLE_UP arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    red_rgba = (255, 68, 68, 255)
    blue_rgba = (59, 130, 246, 255)

    # Red glow - left arrow
    draw_line_thick(draw, 18, 51, 18, 20, red_rgba, 13)
    draw_line_thick(draw, 18, 20, 9, 29, red_rgba, 13)
    draw_line_thick(draw, 18, 20, 27, 29, red_rgba, 13)
    # Red glow - right arrow
    draw_line_thick(draw, 42, 51, 42, 20, red_rgba, 13)
    draw_line_thick(draw, 42, 20, 33, 29, red_rgba, 13)
    draw_line_thick(draw, 42, 20, 51, 29, red_rgba, 13)

    # Blue - left arrow
    draw_line_thick(draw, 18, 51, 18, 20, blue_rgba, 9)
    draw_line_thick(draw, 18, 20, 9, 29, blue_rgba, 9)
    draw_line_thick(draw, 18, 20, 27, 29, blue_rgba, 9)
    # Blue - right arrow
    draw_line_thick(draw, 42, 51, 42, 20, blue_rgba, 9)
    draw_line_thick(draw, 42, 20, 33, 29, blue_rgba, 9)
    draw_line_thick(draw, 42, 20, 51, 29, blue_rgba, 9)

    return img

def generate_double_down_arrow():
    """Generate TREND_DOUBLE_DOWN arrow"""
    img = create_image()
    draw = ImageDraw.Draw(img)

    red_rgba = (255, 68, 68, 255)
    blue_rgba = (59, 130, 246, 255)

    # Red glow - left arrow
    draw_line_thick(draw, 18, 9, 18, 40, red_rgba, 13)
    draw_line_thick(draw, 18, 40, 9, 31, red_rgba, 13)
    draw_line_thick(draw, 18, 40, 27, 31, red_rgba, 13)
    # Red glow - right arrow
    draw_line_thick(draw, 42, 9, 42, 40, red_rgba, 13)
    draw_line_thick(draw, 42, 40, 33, 31, red_rgba, 13)
    draw_line_thick(draw, 42, 40, 51, 31, red_rgba, 13)

    # Blue - left arrow
    draw_line_thick(draw, 18, 9, 18, 40, blue_rgba, 9)
    draw_line_thick(draw, 18, 40, 9, 31, blue_rgba, 9)
    draw_line_thick(draw, 18, 40, 27, 31, blue_rgba, 9)
    # Blue - right arrow
    draw_line_thick(draw, 42, 9, 42, 40, blue_rgba, 9)
    draw_line_thick(draw, 42, 40, 33, 31, blue_rgba, 9)
    draw_line_thick(draw, 42, 40, 51, 31, blue_rgba, 9)

    return img

def rgb_to_rgb565(r, g, b):
    """Convert RGB888 to RGB565"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def image_to_c_array(img, var_name):
    """Convert PIL Image to C array in RGB565 + Alpha format (LVGL v8 format)"""
    # Convert to RGBA if not already
    img_rgba = img.convert('RGBA')
    pixels = img_rgba.load()

    # Build RGB565 + Alpha data (3 bytes per pixel)
    data = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b, a = pixels[x, y]
            rgb565 = rgb_to_rgb565(r, g, b)
            # Store as: RGB565 low byte, RGB565 high byte, Alpha
            data.append(rgb565 & 0xFF)
            data.append((rgb565 >> 8) & 0xFF)
            data.append(a)

    # Generate C code
    c_code = f"// {var_name} - {WIDTH}x{HEIGHT} RGB565 + Alpha (LVGL v8 format)\n"
    c_code += f"static const uint8_t {var_name}_data[{WIDTH * HEIGHT * 3}] = {{\n"

    # Write data in rows of 16 bytes
    for i in range(0, len(data), 16):
        row = data[i:i+16]
        c_code += "    " + ", ".join(f"0x{val:02X}" for val in row)
        if i + 16 < len(data):
            c_code += ","
        c_code += "\n"

    c_code += "};\n\n"

    c_code += f"const lv_img_dsc_t {var_name} = {{\n"
    c_code += f"    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n"
    c_code += f"    .header.always_zero = 0,\n"
    c_code += f"    .header.reserved = 0,\n"
    c_code += f"    .header.w = {WIDTH},\n"
    c_code += f"    .header.h = {HEIGHT},\n"
    c_code += f"    .data_size = {WIDTH * HEIGHT * 3},\n"
    c_code += f"    .data = {var_name}_data,\n"
    c_code += "};\n\n"

    return c_code

def main():
    """Generate all arrow images and output C code"""
    arrows = [
        ("arrow_flat", generate_flat_arrow()),
        ("arrow_45_up", generate_45_up_arrow()),
        ("arrow_45_down", generate_45_down_arrow()),
        ("arrow_single_up", generate_single_up_arrow()),
        ("arrow_single_down", generate_single_down_arrow()),
        ("arrow_double_up", generate_double_up_arrow()),
        ("arrow_double_down", generate_double_down_arrow()),
    ]

    # Generate header file
    header = """/*
 * Generated arrow images for CYGM
 * Stored in flash memory (const arrays)
 * DO NOT EDIT - regenerate with tools/generate_arrow_images.py
 */

#ifndef ARROW_IMAGES_H
#define ARROW_IMAGES_H

#include "lvgl.h"

extern const lv_img_dsc_t arrow_flat;
extern const lv_img_dsc_t arrow_45_up;
extern const lv_img_dsc_t arrow_45_down;
extern const lv_img_dsc_t arrow_single_up;
extern const lv_img_dsc_t arrow_single_down;
extern const lv_img_dsc_t arrow_double_up;
extern const lv_img_dsc_t arrow_double_down;

#endif // ARROW_IMAGES_H
"""

    with open("../main/arrow_images.h", "w") as f:
        f.write(header)

    # Generate C file
    c_file = """/*
 * Generated arrow images for CYGM
 * Stored in flash memory (const arrays)
 * DO NOT EDIT - regenerate with tools/generate_arrow_images.py
 */

#include "arrow_images.h"

"""

    for var_name, img in arrows:
        c_file += image_to_c_array(img, var_name)
        # Save preview PNG
        img.save(f"../docs/arrows/{var_name}.png")

    with open("../main/arrow_images.c", "w") as f:
        f.write(c_file)

    print("Generated arrow_images.h and arrow_images.c")
    print(f"Total flash usage: {7 * WIDTH * HEIGHT * 3} bytes (~{7 * WIDTH * HEIGHT * 3 / 1024:.1f} KB)")
    print("Format: RGB565 + Alpha (LVGL v8 TRUE_COLOR_ALPHA - proper transparency)")

if __name__ == "__main__":
    main()
