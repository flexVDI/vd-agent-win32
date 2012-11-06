/*
   Copyright (C) 2009 Red Hat, Inc.

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

#ifndef _H_VDI_PORT
#define _H_VDI_PORT

#include <windows.h>
#include <stdint.h>

#define MIN(a, b) ((a) > (b) ? (b) : (a))

#define BUF_SIZE    (1024 * 1024)

#define VDI_PORT_BLOCKED    0
#define VDI_PORT_RESET      -1
#define VDI_PORT_ERROR      -2

// Ring notes:
// _end is one after the end of data
// _start==_end means empty ring
// _start-1==_end (modulo) means full ring
// _start-1 is never used
// ring_write & read on right side of the ring (update _end)
// ring_read & write from left (update _start)

typedef struct VDIPortBuffer {
    OVERLAPPED overlap;
    uint8_t* start;
    uint8_t* end;
    bool pending;
    int bytes;
    uint8_t ring[BUF_SIZE];
} VDIPortBuffer;

class VDIPort {
public:
    VDIPort();
    virtual ~VDIPort() {}

    size_t ring_write(const void* buf, size_t size);
    size_t write_ring_free_space();
    size_t ring_read(void* buf, size_t size);
    size_t read_ring_size();
    size_t read_ring_continuous_remaining_size();

    virtual const char *name() = 0;
    virtual bool init() = 0;
    virtual unsigned get_num_events() = 0;
    virtual void fill_events(HANDLE* handles) = 0;
    virtual bool handle_event(int event) = 0;
    virtual int write() = 0;
    virtual int read() = 0;

protected:
    int handle_error();

    VDIPortBuffer _write;
    VDIPortBuffer _read;
};

#endif
