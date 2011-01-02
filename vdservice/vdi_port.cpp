#include "vdlog.h"
#include "vdi_port.h"

VDIPort::VDIPort()
{
    ZeroMemory(&_write, offsetof(VDIPortBuffer, ring));
    _write.start = _write.end = _write.ring;
    ZeroMemory(&_read, offsetof(VDIPortBuffer, ring));
    _read.start = _read.end = _read.ring;
}

size_t VDIPort::read_ring_size()
{
    return (BUF_SIZE + _read.end - _read.start) % BUF_SIZE;
}

size_t VDIPort::write_ring_free_space()
{
    return (BUF_SIZE + _write.start - _write.end - 1) % BUF_SIZE;
}

size_t VDIPort::ring_write(const void* buf, size_t size)
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

size_t VDIPort::read_ring_continuous_remaining_size()
{
    DWORD size;

    if (_read.start <= _read.end) {
        size = MIN(BUF_SIZE - 1, (int)(&_read.ring[BUF_SIZE] - _read.end));
    } else {
        size = (DWORD)(_read.start - _read.end - 1);
    }
    return size;
}

size_t VDIPort::ring_read(void* buf, size_t size)
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

int VDIPort::handle_error()
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
