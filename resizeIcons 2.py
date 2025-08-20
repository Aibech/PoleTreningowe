#!/usr/bin/env python
# -*- coding: utf-8 -*-

"""
Script for converting SVG icons to PNG format with padding.
It preserves original icon size and adds transparent margins to make it square.
Generates multiple sizes: original (with padding) and 64x64, 128x128, 256x256 versions.
"""

import sys
import cairosvg
import io
from PIL import Image
from xml.etree import ElementTree

def get_svg_size(svg_path):
    """Get original SVG dimensions by parsing the SVG file.
    Tries multiple methods:
    1. width/height attributes
    2. viewBox attribute
    Raises ValueError if size cannot be determined"""
    root = ElementTree.parse(svg_path).getroot()
    
    # Try to get from width/height attributes
    width = root.get('width', '').replace('px', '')
    height = root.get('height', '').replace('px', '')
    
    # If width/height not found, try to get from viewBox
    if not (width and height):
        viewbox = root.get('viewBox', '').split()
        if len(viewbox) == 4:
            # viewBox format: "min-x min-y width height"
            width = viewbox[2]
            height = viewbox[3]
    
    # Convert to numbers
    try:
        width = int(float(width))
        height = int(float(height))
    except (ValueError, TypeError):
        raise ValueError(f"Cannot determine SVG size for {svg_path}. The file must have width/height attributes or viewBox defined.")
        
    return width, height

def convert_svg_to_png(svg_path, target_width, target_height):
    """Convert SVG file to PNG format with specified dimensions.
    Uses cairosvg for conversion while maintaining transparency and quality"""
    png_data = cairosvg.svg2png(
        url=svg_path,
        background_color="none",
        output_width=target_width,
        output_height=target_height,
        scale=1.0,
        dpi=36
    )
    return Image.open(io.BytesIO(png_data))

def add_padding(image, target_size):
    """Center the image in a larger square canvas with transparent padding.
    Creates a new square image and copies pixels from original while preserving transparency"""
    orig_width, orig_height = image.size
    new_image = Image.new('RGBA', (target_size, target_size), (0, 0, 0, 0))
    
    x = (target_size - orig_width) // 2
    y = (target_size - orig_height) // 2
    
    # Copy pixels while preserving transparency
    for i in range(orig_width):
        for j in range(orig_height):
            pixel = image.getpixel((i, j))
            if pixel[3] > 0:  # Only copy non-transparent pixels
                new_image.putpixel((x + i, y + j), pixel)
    
    return new_image

def process_icons(svg_path, base_size):
    """Main function to process SVG icons into multiple PNG sizes.
    Steps:
    1. Get original SVG dimensions
    2. Calculate margin percentage based on base size
    3. Generate PNGs in multiple sizes while maintaining aspect ratio and margins"""
    try:
        orig_width, orig_height = get_svg_size(svg_path)
    except ValueError as e:
        print(f"Error: {e}")
        sys.exit(1)
        
    margin_percent = (base_size - max(orig_width, orig_height)) / (2 * max(orig_width, orig_height))
    base_path = svg_path.rsplit('.', 1)[0]
    
    for size in [base_size, 64, 128, 256]:
        # Calculate size while preserving margin proportions
        content_size = int(size / (1 + 2 * margin_percent))
        scale = content_size / max(orig_width, orig_height)
        target_width = int(orig_width * scale)
        target_height = int(orig_height * scale)
        
        # Convert SVG to PNG and add padding
        image = convert_svg_to_png(svg_path, target_width, target_height)
        final_image = add_padding(image, size)
        
        # Save with appropriate filename
        output_path = f"{base_path}{'_' + str(size) if size != base_size else ''}.png"
        final_image.save(output_path, "PNG", optimize=False)
        print(f"Created: {output_path}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python resizeIcons.py path/to/icon.svg base_size")
        sys.exit(1)
    
    process_icons(sys.argv[1], int(sys.argv[2]))
