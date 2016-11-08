/*
   Copyright (C) 2013-2017 Red Hat, Inc.

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

#include <spice/macros.h>
#include <memory>
#include <vector>

#include "vdcommon.h"
#include "image.h"

ImageCoder *create_bitmap_coder();
ImageCoder *create_png_coder();

static ImageCoder *get_coder(uint32_t vdagent_type)
{
    switch (vdagent_type) {
    case VD_AGENT_CLIPBOARD_IMAGE_BMP:
        return create_bitmap_coder();
    case VD_AGENT_CLIPBOARD_IMAGE_PNG:
        return create_png_coder();
    }
    return NULL;
}

HANDLE get_image_handle(const VDAgentClipboard& clipboard, uint32_t size, UINT& format)
{
    std::unique_ptr<ImageCoder> coder(get_coder(clipboard.type));
    if (!coder) {
        return NULL;
    }

    format = CF_DIB;
    size_t dib_size = coder->get_dib_size(clipboard.data, size);
    if (!dib_size) {
        return NULL;
    }
    HANDLE clip_data = GlobalAlloc(GMEM_MOVEABLE, dib_size);
    if (clip_data) {
        uint8_t* dst = (uint8_t*)GlobalLock(clip_data);
        if (!dst) {
            GlobalFree(clip_data);
            return NULL;
        }
        coder->get_dib_data(dst, clipboard.data, size);
        GlobalUnlock(clip_data);
    }
    return clip_data;
}

uint8_t* get_raw_clipboard_image(const VDAgentClipboardRequest& clipboard_request,
                                 HANDLE clip_data, long& new_size)
{
    new_size = 0;

    if (GetObjectType(clip_data) != OBJ_BITMAP) {
        return NULL;
    }

    std::unique_ptr<ImageCoder> coder(get_coder(clipboard_request.type));
    if (!coder) {
        return NULL;
    }

    HPALETTE pal = 0;
    if (IsClipboardFormatAvailable(CF_PALETTE)) {
        pal = (HPALETTE)GetClipboardData(CF_PALETTE);
    }

    // extract DIB
    BITMAP bitmap;
    GetObject(clip_data, sizeof(bitmap), &bitmap);

    struct {
        BITMAPINFOHEADER head;
        RGBQUAD colors[256];
    } info;

    BITMAPINFOHEADER& head(info.head);
    memset(&head, 0, sizeof(head));
    head.biSize = sizeof(head);
    head.biWidth = bitmap.bmWidth;
    head.biHeight = bitmap.bmHeight;
    head.biPlanes = bitmap.bmPlanes;
    head.biBitCount = bitmap.bmBitsPixel >= 16 ? 24 : bitmap.bmBitsPixel;
    head.biCompression = BI_RGB;

    HDC dc = GetDC(NULL);
    HPALETTE old_pal = NULL;
    if (pal) {
        old_pal = (HPALETTE)SelectObject(dc, pal);
        RealizePalette(dc);
    }
    size_t stride = compute_dib_stride(head.biWidth, head.biBitCount);
    std::vector<uint8_t> bits(stride * head.biHeight);
    int res = GetDIBits(dc, (HBITMAP) clip_data, 0, head.biHeight,
                        &bits[0], (LPBITMAPINFO)&info, DIB_RGB_COLORS);
    if (pal) {
        SelectObject(dc, old_pal);
    }
    ReleaseDC(NULL, dc);
    if (!res) {
        return NULL;
    }

    // convert DIB to desired format
    return coder->from_bitmap(*(LPBITMAPINFO)&info, &bits[0], new_size);
}

void free_raw_clipboard_image(uint8_t *data)
{
    free(data);
}

class BitmapCoder: public ImageCoder
{
public:
    BitmapCoder() {};
    size_t get_dib_size(const uint8_t *data, size_t size);
    void get_dib_data(uint8_t *dib, const uint8_t *data, size_t size);
    uint8_t *from_bitmap(const BITMAPINFO& info, const void *bits, long &size);
};

size_t BitmapCoder::get_dib_size(const uint8_t *data, size_t size)
{
    if (memcmp(data, "BM", 2) == 0)
        return size > 14 ? size - 14 : 0;
    return size;
}

void BitmapCoder::get_dib_data(uint8_t *dib, const uint8_t *data, size_t size)
{
    // just strip the file header if present, images can be either BMP or DIB
    size_t new_size = get_dib_size(data, size);
    memcpy(dib, data + (size - new_size), new_size);
}

uint8_t *BitmapCoder::from_bitmap(const BITMAPINFO& info, const void *bits, long &size)
{
    const BITMAPINFOHEADER& head(info.bmiHeader);

    const DWORD max_palette_colors = head.biBitCount <= 8 ? 1 << head.biBitCount : 0;
    size_t palette_size = sizeof(RGBQUAD) * std::min(head.biClrUsed, max_palette_colors);

    const size_t stride = compute_dib_stride(head.biWidth, head.biBitCount);
    const size_t image_size = stride * head.biHeight;
    size = sizeof(head) + palette_size + image_size;

    uint8_t *data = (uint8_t *) malloc(size);
    if (!data) {
        return NULL;
    }
    memcpy(data, &info, sizeof(head) + palette_size);
    memcpy(data + sizeof(head) + palette_size, bits, image_size);
    return data;
}

ImageCoder *create_bitmap_coder()
{
    return new BitmapCoder();
}

// TODO
ImageCoder *create_png_coder()
{
    return NULL;
}
