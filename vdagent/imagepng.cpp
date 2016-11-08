/*
   Copyright (C) 2017 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "vdcommon.h"

#include <png.h>
#include <algorithm>
#include <vector>

#include "imagepng.h"

class PngCoder: public ImageCoder
{
public:
    PngCoder() {};
    size_t get_dib_size(const uint8_t *data, size_t size);
    void get_dib_data(uint8_t *dib, const uint8_t *data, size_t size);
    uint8_t *from_bitmap(const BITMAPINFO& info, const void *bits, long &size);
private:
    size_t convert_to_dib(uint8_t *out_buf, const uint8_t *data, size_t size);
};

struct ReadBufferIo {
    const uint8_t *buf;
    uint32_t pos, size;
    ReadBufferIo(const uint8_t *_buf, uint32_t _size):
        buf(_buf), pos(0), size(_size)
    {}
};

static void read_from_bufio(png_structp png, png_bytep out, png_size_t size)
{
    ReadBufferIo& io(*(ReadBufferIo*)png_get_io_ptr(png));
    if (io.pos + size > io.size)
        png_error(png, "read past end");
    memcpy(out, io.buf+io.pos, size);
    io.pos += size;
}

struct WriteBufferIo {
    uint8_t *buf;
    uint32_t pos, size;
    WriteBufferIo():
        buf(NULL), pos(0), size(0)
    {}
    ~WriteBufferIo() { free(buf); }
    uint8_t *release() {
        uint8_t *res = buf;
        buf = NULL;
        pos = size = 0;
        return res;
    }
};

static void write_to_bufio(png_structp png, png_bytep in, png_size_t size)
{
    WriteBufferIo& io(*(WriteBufferIo*)png_get_io_ptr(png));
    if (io.pos + size > io.size) {
        uint32_t new_size = io.size ? io.size * 2 : 4096;
        while (io.pos + size >= new_size) {
            new_size *= 2;
        }
        uint8_t *p = (uint8_t*) realloc(io.buf, new_size);
        if (!p)
            png_error(png, "out of memory");
        io.buf = p;
        io.size = new_size;
    }
    memcpy(io.buf+io.pos, in, size);
    io.pos += size;
}

static void flush_bufio(png_structp png)
{
}

size_t PngCoder::get_dib_size(const uint8_t *data, size_t size)
{
    return convert_to_dib(NULL, data, size);
}

typedef void line_fixup_t(uint8_t *line, unsigned int width);

static void line_fixup_none(uint8_t *line, unsigned int width)
{
}

static void line_fixup_2bpp_to_4bpp(uint8_t *line, unsigned int width)
{
    width = (width + 3) / 4u;
    while (width--) {
        uint8_t from = line[width];
        line[width*2+1] = ((from & 0x03) << 0) | ((from & 0x0c) << 2);
        line[width*2+0] = ((from & 0x30) >> 4) | ((from & 0xc0) >> 2);
    }
}

size_t PngCoder::convert_to_dib(uint8_t *out_buf, const uint8_t *data, size_t size)
{
    ReadBufferIo io(data, size);

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return 0;

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }

    png_set_read_fn(png, &io, read_from_bufio);

    png_read_info(png, info);

    // not so much precision is supported
    unsigned int bits = png_get_bit_depth(png, info);
    if (bits == 16)
        png_set_strip_16(png);

    unsigned int out_bits;
    bool is_gray = false;
    line_fixup_t *line_fixup = line_fixup_none;
    switch (png_get_color_type(png, info)) {
    case PNG_COLOR_TYPE_GRAY:
        is_gray = true;
        if (bits == 16) {
            out_bits = 8;
        } else if (bits == 2) {
            line_fixup = line_fixup_2bpp_to_4bpp;
            out_bits = 4;
        } else {
            out_bits = bits;
        }
        break;
    case PNG_COLOR_TYPE_PALETTE:
        // should return 1, 4 and 8, BMP does not support 2
        out_bits = bits;
        if (bits == 2) {
            line_fixup = line_fixup_2bpp_to_4bpp;
            out_bits = 4;
        }
        break;
    case PNG_COLOR_TYPE_RGB:
        png_set_bgr(png);
        out_bits = 24;
        break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
        png_set_bgr(png);
        out_bits = 24;
        png_set_strip_alpha(png);
        break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
        is_gray = true;
        // gray with alpha should always be 8 bit but make it sure
        // in case format change
        png_set_expand_gray_1_2_4_to_8(png);
        out_bits = 8;
        png_set_strip_alpha(png);
        break;
    default:
        png_error(png, "PNG color type not supported");
        break;
    }

    const unsigned int width = png_get_image_width(png, info);
    const unsigned int height = png_get_image_height(png, info);
    const size_t stride = compute_dib_stride(width, out_bits);
    const size_t image_size = stride * height;
    int palette_colors;
    // no palette
    if (out_bits > 8) {
        palette_colors = 0;
    // 2 bit PNG converted to 4 bit BMP
    } else if (bits == 2) {
        palette_colors = 4;
    } else {
        palette_colors = 1 << out_bits;
    }
    const size_t palette_size = palette_colors * sizeof(RGBQUAD);
    const size_t dib_size = sizeof(BITMAPINFOHEADER) + palette_size + image_size;

    // just called to get the size, return the information
    if (!out_buf) {
        png_destroy_read_struct(&png, &info, NULL);
        return dib_size;
    }

    // TODO tests
    // bits, 1, 2, 4, 8, 16
    // all color types
    // alpha/not alpha
    // indexed with not all colors

    // fill header
    BITMAPINFOHEADER& head(*(BITMAPINFOHEADER *)out_buf);
    memset(&head, 0, sizeof(head));
    head.biSize = sizeof(head);
    head.biWidth = width;
    head.biHeight = height;
    head.biPlanes = 1;
    head.biBitCount = out_bits;
    head.biCompression = BI_RGB;
    head.biSizeImage = image_size;

    // copy palette
    RGBQUAD *rgb = (RGBQUAD *)(out_buf + sizeof(BITMAPINFOHEADER));
    if (is_gray) {
        const unsigned int mult = 255 / (palette_colors - 1);
        for (int color = 0; color < palette_colors; ++color) {
            rgb->rgbBlue = rgb->rgbGreen = rgb->rgbRed = color * mult;
            rgb->rgbReserved = 0;
            ++rgb;
        }
        head.biClrUsed = palette_colors;
    } else if (out_bits <= 8) {
        png_colorp palette = NULL;
        int num_palette;
        if (!png_get_PLTE(png, info, &palette, &num_palette)) {
            png_error(png, "error getting palette");
        }
        for (int color = 0; color < palette_colors; ++color) {
            if (color < num_palette) {
                rgb->rgbBlue = palette->blue;
                rgb->rgbGreen = palette->green;
                rgb->rgbRed = palette->red;
            } else {
                rgb->rgbBlue = rgb->rgbGreen = rgb->rgbRed = 0;
            }
            rgb->rgbReserved = 0;
            ++rgb;
            ++palette;
        }
        head.biClrUsed = palette_colors;
    }

    // now do the actual conversion!
    uint8_t *dst = out_buf + sizeof(BITMAPINFOHEADER) + palette_size + image_size;
    for (unsigned int row = 0; row < height; ++row) {
        ((uint32_t*)dst)[-1] = 0; // padding
        dst -= stride;
        png_read_row(png, dst, NULL);
        line_fixup(dst, width);
    }

    png_destroy_read_struct(&png, &info, NULL);
    return dib_size;
}

void PngCoder::get_dib_data(uint8_t *dib, const uint8_t *data, size_t size)
{
    convert_to_dib(dib, data, size);
}

uint8_t *PngCoder::from_bitmap(const BITMAPINFO& bmp_info, const void *bits, long &size)
{
    // this vector is here to avoid leaking resources if libpng use setjmp/longjmp
    std::vector<png_color> palette;
    WriteBufferIo io;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png)
        return 0;

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, &info);
        return 0;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return 0;
    }

    png_set_write_fn(png, &io, write_to_bufio, flush_bufio);

    const BITMAPINFOHEADER& head(bmp_info.bmiHeader);
    int color_type;
    int out_bits = head.biBitCount;
    switch (out_bits) {
    case 1:
    case 4:
    case 8:
        color_type = PNG_COLOR_TYPE_PALETTE;
        break;
    case 24:
    case 32:
        png_set_bgr(png);
        color_type = PNG_COLOR_TYPE_RGB;
        break;
    default:
        png_error(png, "BMP bit count not supported");
        break;
    }
    // TODO detect gray
    png_set_IHDR(png, info, head.biWidth, head.biHeight,
                 out_bits > 8 ? 8 : out_bits, color_type,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    // palette
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        palette.resize(head.biClrUsed);
        const RGBQUAD *rgb = bmp_info.bmiColors;
        for (unsigned int color = 0; color < head.biClrUsed; ++color) {
            palette[color].red = rgb->rgbRed;
            palette[color].green = rgb->rgbGreen;
            palette[color].blue = rgb->rgbBlue;
            ++rgb;
        }
        png_set_PLTE(png, info, &palette[0], palette.size());
    }

    png_write_info(png, info);

    const unsigned int width = head.biWidth;
    const unsigned int height = head.biHeight;
    const size_t stride = compute_dib_stride(width, out_bits);
    const size_t image_size = stride * height;

    // now do the actual conversion!
    const uint8_t *src = (const uint8_t*)bits + image_size;
    for (unsigned int row = 0; row < height; ++row) {
        src -= stride;
        png_write_row(png, src);
    }
    png_write_end(png, NULL);

    png_destroy_write_struct(&png, &info);
    size = io.pos;
    return io.release();
}

ImageCoder *create_png_coder()
{
    return new PngCoder();
}
