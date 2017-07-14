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

#undef NDEBUG
#include <assert.h>
#include <vector>

#include "vdcommon.h"
#include "image.h"
#include "imagepng.h"

int main(int argc, char **argv)
{
    ImageCoder *coder = create_png_coder();

    assert(coder);
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <in-image> [<out-bmp> [<out-png>]]\n", argv[0]);
        return 1;
    }

    // read all file into memory
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long len = ftell(f);
    assert(len > 0);
    assert(fseek(f, 0, SEEK_SET) == 0);

    std::vector<uint8_t> data(len);
    assert(fread(&data[0], 1, len, f) == (unsigned long) len);
    fclose(f);

    size_t dib_size = coder->get_dib_size(&data[0], len);
    assert(dib_size);
    std::vector<uint8_t> out(dib_size);
    memset(&out[0], 0xcc, dib_size);
    coder->get_dib_data(&out[0], &data[0], len);

    // looks like many tools wants this header so craft it
    BITMAPFILEHEADER head;
    memset(&head, 0, sizeof(head));
    head.bfType = 'B'+'M'*256u;
    head.bfSize = sizeof(head) + dib_size;
    BITMAPINFOHEADER& info(*(BITMAPINFOHEADER*)&out[0]);
    head.bfOffBits = sizeof(head) + sizeof(BITMAPINFOHEADER) + 4 * info.biClrUsed;

    f = fopen(argc > 2 ? argv[2] : "out.bmp", "wb");
    assert(f);
    assert(fwrite(&head, 1, sizeof(head), f) == sizeof(head));
    assert(fwrite(&out[0], 1, dib_size, f) == dib_size);
    fclose(f);

    // convert back to PNG
    long png_size = 0;
    uint8_t *png = coder->from_bitmap(*((BITMAPINFO*)&out[0]), &out[sizeof(BITMAPINFOHEADER) + 4 * info.biClrUsed], png_size);
    assert(png && png_size > 0);

    f = fopen(argc > 3 ? argv[3] : "out.png", "wb");
    assert(f);
    assert(fwrite(png, 1, png_size, f) == (unsigned long) png_size);
    fclose(f);
    free(png);
    png = NULL;

    return 0;
}

