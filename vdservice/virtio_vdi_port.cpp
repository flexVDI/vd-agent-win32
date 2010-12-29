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

#define VIOSERIAL_PORT_PATH                 L"\\\\.\\Global\\com.redhat.spice.0"

// Current limitation of virtio-serial windows driver (RHBZ 617000)
#define VIOSERIAL_PORT_MAX_WRITE_BYTES      2048

#define MIN(a, b) ((a) > (b) ? (b) : (a))

VirtioVDIPort* VirtioVDIPort::_singleton;

VirtioVDIPort::VirtioVDIPort()
    : _handle (INVALID_HANDLE_VALUE)
{
    ZeroMemory(&_write, offsetof(VDIPortBuffer, ring));
    _write.start = _write.end = _write.ring;
    ZeroMemory(&_read, offsetof(VDIPortBuffer, ring));
    _read.start = _read.end = _read.ring;
    _singleton = this;
}

VirtioVDIPort::~VirtioVDIPort()
{
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(_handle);
    }
    if (_read.overlap.hEvent) {
        CloseHandle(_read.overlap.hEvent);
    }
    if (_write.overlap.hEvent) {
        CloseHandle(_write.overlap.hEvent);
    }
}

bool VirtioVDIPort::init()
{
    _handle = CreateFile(VIOSERIAL_PORT_PATH, GENERIC_READ | GENERIC_WRITE , 0, NULL,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (_handle == INVALID_HANDLE_VALUE) {
        vd_printf("CreateFile() %s failed: %u", VIOSERIAL_PORT_PATH, GetLastError());
        return false;
    }
    _write.overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (_write.overlap.hEvent == NULL) {
        vd_printf("CreateEvent() failed: %u", GetLastError());
        return false;
    }
    _read.overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (_read.overlap.hEvent == NULL) {
        vd_printf("CreateEvent() failed: %u", GetLastError());
        return false;
    }
    return true;
}

size_t VirtioVDIPort::write_ring_free_space()
{
    return (BUF_SIZE + _write.start - _write.end - 1) % BUF_SIZE;
}

size_t VirtioVDIPort::ring_write(const void* buf, size_t size)
{
    size_t free_size = (BUF_SIZE + _write.start - _write.end - 1) % BUF_SIZE;
    size_t n;

    if (size > free_size) {
        size = free_size;
    }
    if (_write.end < _write.start) {
        memcpy(_write.end, buf, size);
    } else {
        n = MIN(size, (size_t)(&_write.ring[BUF_SIZE] - _write.end));
        memcpy(_write.end, buf, n);
        if (size > n) {
            memcpy(_write.ring, (uint8_t*)buf + n, size - n);
        }
    }
    _write.end = _write.ring + (_write.end - _write.ring + size) % BUF_SIZE;
    return size;
}

int VirtioVDIPort::write()
{
    int size;
    int ret;

    //FIXME: return VDI_PORT_NO_DATA
    if (_write.start == _write.end) {
        return 0;
    }
    if (!_write.pending) {
        if (_write.start < _write.end) {
            size = (int)(_write.end - _write.start);
        } else {
            size = (int)(&_write.ring[BUF_SIZE] - _write.start);
        }
        size = MIN(size, VIOSERIAL_PORT_MAX_WRITE_BYTES);
        _write.pending = true;
        if (WriteFile(_handle, _write.start, size, NULL, &_write.overlap)) {
            write_completion();
        } if (GetLastError() != ERROR_IO_PENDING) {
            return handle_error();
        }
    }
    ret = _write.bytes;
    _write.bytes = 0;
    return ret;
}

void VirtioVDIPort::write_completion()
{
    DWORD bytes;

    if (!_write.pending) {
        return;
    }
    if (!GetOverlappedResult(_handle, &_write.overlap, &bytes, FALSE)) {
        vd_printf("GetOverlappedResult failed: %u", GetLastError());
        return;
    }
    _write.start = _write.ring + (_write.start - _write.ring + bytes) % BUF_SIZE;
    _write.bytes = bytes;
    _write.pending = false;
}

size_t VirtioVDIPort::read_ring_size()
{
    return (BUF_SIZE + _read.end - _read.start) % BUF_SIZE;
}

size_t VirtioVDIPort::read_ring_continuous_remaining_size()
{
    DWORD size;

    if (_read.start <= _read.end) {
        size = MIN(BUF_SIZE - 1, (int)(&_read.ring[BUF_SIZE] - _read.end));
    } else {
        size = (DWORD)(_read.start - _read.end - 1);
    }
    return size;
}

size_t VirtioVDIPort::ring_read(void* buf, size_t size)
{
    size_t n;
    size_t m = 0;

    if (_read.start == _read.end) {
        return 0;
    }
    if (_read.start < _read.end) {
        n = MIN(size, (size_t)(_read.end - _read.start));
        memcpy(buf, _read.start, n);
    } else {
        n = MIN(size, (size_t)(&_read.ring[BUF_SIZE] - _read.start));
        memcpy(buf, _read.start, n);
        if (size > n) {
            m = MIN(size - n, (size_t)(_read.end - _read.ring));
            memcpy((uint8_t*)buf + n, _read.ring, m);
        }
    }
    _read.start = _read.ring + (_read.start - _read.ring + n + m) % BUF_SIZE;
    return n + m;
}

int VirtioVDIPort::read()
{
    int size;
    int ret;

    if (!_read.pending) {
        //FIXME: read_ring_continuous_remaining_size? return VDI_PORT_BUFFER_FULL
        if ((_read.end - _read.ring + 1) % BUF_SIZE == _read.start - _read.ring) {
            vd_printf("DEBUG: buffer full");
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
        _read.pending = true;
        if (ReadFile(_handle, _read.end, size, NULL, &_read.overlap)) {
            read_completion();
        } else if (GetLastError() != ERROR_IO_PENDING) {
            return handle_error();
        }
    }
    ret = _read.bytes;
    _read.bytes = 0;
    return ret;
}

void VirtioVDIPort::read_completion()
{
    DWORD bytes;

    if (!GetOverlappedResult(_handle, &_read.overlap, &bytes, FALSE) &&
                                      GetLastError() != ERROR_MORE_DATA) {
        vd_printf("GetOverlappedResult failed: %u", GetLastError());
        return;
    }
    _read.end = _read.ring + (_read.end - _read.ring + bytes) % BUF_SIZE;
    _read.bytes = bytes;
    _read.pending = false;
}

int VirtioVDIPort::handle_error()
{
    switch (GetLastError()) {
    case ERROR_CONNECTION_INVALID:
        vd_printf("port reset");
        _write.start = _write.end = _write.ring;
        _read.start = _read.end = _read.ring;
        return VDI_PORT_RESET;
    default:
        vd_printf("port io failed: %u", GetLastError());
        return VDI_PORT_ERROR;
    }
}
