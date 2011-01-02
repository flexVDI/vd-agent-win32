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

#ifndef _H_PCI_VDI_PORT
#define _H_PCI_VDI_PORT

#include "vdi_port.h"

#define BUF_READ    (1 << 0)
#define BUF_WRITE   (1 << 1)
#define BUF_ALL     (BUF_READ | BUF_WRITE)

enum {
    PCI_VDI_PORT_EVENT = 0,
    PCI_VDI_PORT_EVENT_COUNT
};

class PCIVDIPort : public VDIPort {
public:
    PCIVDIPort();
    ~PCIVDIPort();
    virtual bool init();
    virtual const char *name() {
        return "PCIVDIPort";
    }
    virtual int write();
    virtual int read();
    virtual unsigned get_num_events() { return PCI_VDI_PORT_EVENT_COUNT; }
    virtual void fill_events(HANDLE* handle);
    virtual void handle_event(int event);

private:
    HANDLE _handle;
    HANDLE _event;
};

// Ring notes:
// _end is one after the end of data
// _start==_end means empty ring
// _start-1==_end (modulo) means full ring
// _start-1 is never used
// ring_write & read on right side of the ring (update _end)
// ring_read & write from left (update _start)

#endif
