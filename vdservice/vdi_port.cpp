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
#include "vdi_port.h"
#include "vdlog.h"

#define VDI_PORT_DEV_NAME   TEXT("\\\\.\\VDIPort")
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     0
#define FILE_ANY_ACCESS     0

#define CTL_CODE(DeviceType, Function, Method, Access) (                   \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) \
)

#define FIRST_AVAIL_IO_FUNC 0x800
#define RED_TUNNEL_CTL_FUNC FIRST_AVAIL_IO_FUNC

#define IOCTL_RED_TUNNEL_SET_EVENT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, RED_TUNNEL_CTL_FUNC, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define MIN(a, b) ((a) > (b) ? (b) : (a))

VDIPort::VDIPort()
    : _handle (INVALID_HANDLE_VALUE)
    , _event (NULL)
    , _write_start (_write_ring)
    , _write_end (_write_ring)
    , _read_start (_read_ring)
    , _read_end (_read_ring)
{
}

VDIPort::~VDIPort()
{
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(_handle);
    }
    if (_event) {
        CloseHandle(_event);
    }
}

bool VDIPort::init()
{
    DWORD io_ret_len;
    _handle = CreateFile(VDI_PORT_DEV_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (_handle == INVALID_HANDLE_VALUE) {
        vd_printf("CreateFile() failed: %u", GetLastError());
        return false;
    }
    _event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (_event == NULL) {
        vd_printf("CreateEvent() failed: %u", GetLastError());
        return false;
    }
    if (!DeviceIoControl(_handle, IOCTL_RED_TUNNEL_SET_EVENT, &_event, sizeof(_event),
                         NULL, 0, &io_ret_len, NULL)) {
        vd_printf("DeviceIoControl() failed: %u", GetLastError());
        return false;
    }
    return true;
}

size_t VDIPort::ring_write(const void* buf, size_t size)
{
    size_t free_size = (BUF_SIZE + _write_start - _write_end - 1) % BUF_SIZE;
    size_t n;

    if (size > free_size) {
        size = free_size;
    }
    if (_write_end < _write_start) {
        memcpy(_write_end, buf, size);
    } else {
        n = MIN(size, (size_t)(&_write_ring[BUF_SIZE] - _write_end));
        memcpy(_write_end, buf, n);
        if (size > n) {
            memcpy(_write_ring, (uint8_t*)buf + n, size - n);
        }
    }
    _write_end = _write_ring + (_write_end - _write_ring + size) % BUF_SIZE;
    return size;
}

int VDIPort::write()
{
    int size;
    int n;

    if (_write_start == _write_end) {
        return 0;
    }
    if (_write_start < _write_end) {
        size = (int)(_write_end - _write_start);
    } else {
        size = (int)(&_write_ring[BUF_SIZE] - _write_start);
    }
    if (!WriteFile(_handle, _write_start, size, (LPDWORD)&n, NULL)) {
        return handle_error();
    }
    _write_start = _write_ring + (_write_start - _write_ring + n) % BUF_SIZE;
    return n;
}

size_t VDIPort::read_ring_size()
{
    return (BUF_SIZE + _read_end - _read_start) % BUF_SIZE;
}

size_t VDIPort::ring_read(void* buf, size_t size)
{
    size_t n;
    size_t m = 0;

    if (_read_start == _read_end) {
        return 0;
    }
    if (_read_start < _read_end) {
        n = MIN(size, (size_t)(_read_end - _read_start));
        memcpy(buf, _read_start, n);
    } else {
        n = MIN(size, (size_t)(&_read_ring[BUF_SIZE] - _read_start));
        memcpy(buf, _read_start, n);
        if (size > n) {
            m = MIN(size - n, (size_t)(_read_end - _read_ring));
            memcpy((uint8_t*)buf + n, _read_ring, m);
        }
    }
    _read_start = _read_ring + (_read_start - _read_ring + n + m) % BUF_SIZE;
    return n + m;
}

int VDIPort::read()
{
    int size;
    int n;

    if ((_read_end - _read_ring + 1) % BUF_SIZE == _read_start - _read_ring) {
        return 0;
    }
    if (_read_start == _read_end) {
        _read_start = _read_end = _read_ring;
    }
    if (_read_start <= _read_end) {
        size = MIN(BUF_SIZE - 1, (int)(&_read_ring[BUF_SIZE] - _read_end));
    } else {
        size = (int)(_read_start - _read_end - 1);
    }
    if (!ReadFile(_handle, _read_end, size, (LPDWORD)&n, NULL)) {
        return handle_error();
    }
    _read_end = _read_ring + (_read_end - _read_ring + n) % BUF_SIZE;
    return n;
}

int VDIPort::handle_error()
{
    switch (GetLastError()) {
    case ERROR_CONNECTION_INVALID:
        vd_printf("port reset");
        _write_start = _write_end = _write_ring;
        _read_start = _read_end = _read_ring;
        return VDI_PORT_RESET;
    default:
        vd_printf("port io failed: %u", GetLastError());
        return VDI_PORT_ERROR;
    }
}
