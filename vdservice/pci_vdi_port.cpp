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

#include "stdio.h"
#include "pci_vdi_port.h"
#include "vdlog.h"

#define VDI_PORT_DEV_NAME   TEXT("\\\\.\\VDIPort")
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     0
#define FILE_ANY_ACCESS     0

#ifndef CTL_CODE
//With mingw, this is defined in winioctl.h
#define CTL_CODE(DeviceType, Function, Method, Access) (                   \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)
#endif

#define FIRST_AVAIL_IO_FUNC 0x800
#define RED_TUNNEL_CTL_FUNC FIRST_AVAIL_IO_FUNC

#define IOCTL_RED_TUNNEL_SET_EVENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, RED_TUNNEL_CTL_FUNC, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define MIN(a, b) ((a) > (b) ? (b) : (a))

PCIVDIPort::PCIVDIPort()
    : _handle (INVALID_HANDLE_VALUE)
    , _event (NULL)
{
}

PCIVDIPort::~PCIVDIPort()
{
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(_handle);
    }
    if (_event) {
        CloseHandle(_event);
    }
}

void PCIVDIPort::fill_events(HANDLE* handles) {
    handles[PCI_VDI_PORT_EVENT] = _event;
}

bool PCIVDIPort::init()
{
    DWORD io_ret_len;
    _handle = CreateFile(VDI_PORT_DEV_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (_handle == INVALID_HANDLE_VALUE) {
        vd_printf("CreateFile() failed: %lu", GetLastError());
        return false;
    }
    _event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (_event == NULL) {
        vd_printf("CreateEvent() failed: %lu", GetLastError());
        return false;
    }
    if (!DeviceIoControl(_handle, IOCTL_RED_TUNNEL_SET_EVENT, &_event, sizeof(_event),
                         NULL, 0, &io_ret_len, NULL)) {
        vd_printf("DeviceIoControl() failed: %lu", GetLastError());
        return false;
    }
    return true;
}

int PCIVDIPort::write()
{
    int size;
    int n;

    if (_write.start == _write.end) {
        return 0;
    }
    if (_write.start < _write.end) {
        size = (int)(_write.end - _write.start);
    } else {
        size = (int)(&_write.ring[BUF_SIZE] - _write.start);
    }
    if (!WriteFile(_handle, _write.start, size, (LPDWORD)&n, NULL)) {
        return handle_error();
    }
    _write.start = _write.ring + (_write.start - _write.ring + n) % BUF_SIZE;
    return n;
}

int PCIVDIPort::read()
{
    int size;
    int n;

    if ((_read.end - _read.ring + 1) % BUF_SIZE == _read.start - _read.ring) {
        return 0;
    }
    if (_read.start == _read.end) {
        _read.start = _read.end = _read.ring;
    }
    if (_read.start <= _read.end) {
        size = MIN(BUF_SIZE - 1, (int)(&_read.ring[BUF_SIZE] - _read.end));
    } else {
        size = (int)(_read.start - _read.end - 1);
    }
    if (!ReadFile(_handle, _read.end, size, (LPDWORD)&n, NULL)) {
        return handle_error();
    }
    _read.end = _read.ring + (_read.end - _read.ring + n) % BUF_SIZE;
    return n;
}

void PCIVDIPort::handle_event(int event)
{
    // do nothing - the event merely serves to wake us up, then we call read/write
    // at VDService::execute start of while(_running) loop.
}
