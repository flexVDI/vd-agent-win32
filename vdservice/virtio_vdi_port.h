#ifndef _H_VIRTIO_VDI_PORT
#define _H_VIRTIO_VDI_PORT

#include "vdi_port.h"

class VirtioVDIPort : public VDIPort {
public:
    VirtioVDIPort();
    ~VirtioVDIPort();
    virtual const char *name() { return "VirtioVDIPort"; }
    virtual bool init();
    virtual unsigned get_num_events() { return 2; }
    virtual void fill_events(HANDLE *handle) {
        handle[0] = _write.overlap.hEvent;
        handle[1] = _read.overlap.hEvent;
    }
    virtual void handle_event(int event) {
        switch (event) {
            case 0: write_completion(); break;
            case 1: read_completion(); break;
        }
    }
    virtual int write();
    virtual int read();

private:
    void write_completion();
    void read_completion();

private:
    static VirtioVDIPort* _singleton;
    HANDLE _handle;
};

// Ring notes:
// _end is one after the end of data
// _start==_end means empty ring
// _start-1==_end (modulo) means full ring
// _start-1 is never used
// ring_write & read on right side of the ring (update _end)
// ring_read & write from left (update _start)


#endif //_H_VIRTIO_VDI_PORT