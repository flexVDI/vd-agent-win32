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

#include "vdcommon.h"
#include "image.h"

#include "ximage.h"

typedef struct ImageType {
    uint32_t type;
    DWORD cximage_format;
} ImageType;

static const ImageType image_types[] = {
    {VD_AGENT_CLIPBOARD_IMAGE_PNG, CXIMAGE_FORMAT_PNG},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, CXIMAGE_FORMAT_BMP},
};

static DWORD get_cximage_format(uint32_t type)
{
    for (unsigned int i = 0; i < SPICE_N_ELEMENTS(image_types); i++) {
        if (image_types[i].type == type) {
            return image_types[i].cximage_format;
        }
    }
    return 0;
}

HANDLE get_image_handle(const VDAgentClipboard& clipboard, uint32_t size, UINT&)
{
    HANDLE clip_data;
    DWORD cximage_format = get_cximage_format(clipboard.type);
    ASSERT(cximage_format);
    CxImage image((BYTE*)clipboard.data, size, cximage_format);
    clip_data = image.CopyToHandle();
    return clip_data;
}

uint8_t* get_raw_clipboard_image(const VDAgentClipboardRequest& clipboard_request,
                                 HANDLE clip_data, long& new_size)
{
    new_size = 0;

    CxImage image;
    uint8_t *new_data = NULL;
    DWORD cximage_format = get_cximage_format(clipboard_request.type);
    HPALETTE pal = 0;

    ASSERT(cximage_format);
    if (IsClipboardFormatAvailable(CF_PALETTE)) {
        pal = (HPALETTE)GetClipboardData(CF_PALETTE);
    }
    if (!image.CreateFromHBITMAP((HBITMAP)clip_data, pal)) {
        vd_printf("Image create from handle failed");
        return NULL;
    }
    if (!image.Encode(new_data, new_size, cximage_format)) {
        vd_printf("Image encode to type %u failed", clipboard_request.type);
        return NULL;
    }
    vd_printf("Image encoded to %lu bytes", new_size);
    return new_data;
}

void free_raw_clipboard_image(uint8_t *data)
{
    // this is really just a free however is better to make
    // the free from CxImage code as on Windows the free
    // can be different between libraries
    CxImage image;
    image.FreeMemory(data);
}
