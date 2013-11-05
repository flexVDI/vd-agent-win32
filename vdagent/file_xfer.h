/*
   Copyright (C) 2013 Red Hat, Inc.

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

#ifndef _H_FILE_XFER
#define _H_FILE_XFER

#include <map>
#include "vdcommon.h"

typedef struct ALIGN_VC FileXferTask {
    FileXferTask(HANDLE _handle, uint64_t _size, char* _name):
    handle(_handle), size(_size), pos(0) {
        // FIXME: should raise an error if name is too long..
        strncpy(name, _name, sizeof(name) - 1);
    }
    HANDLE handle;
    uint64_t size;
    uint64_t pos;
    char name[MAX_PATH];
} ALIGN_GCC FileXferTask;

typedef std::map<uint32_t, FileXferTask*> FileXferTasks;

class FileXfer {
public:
    ~FileXfer();
    bool dispatch(VDAgentMessage* msg, VDAgentFileXferStatusMessage* status);

private:
    void handle_start(VDAgentFileXferStartMessage* start, VDAgentFileXferStatusMessage* status);
    bool handle_data(VDAgentFileXferDataMessage* data, VDAgentFileXferStatusMessage* status);
    void handle_status(VDAgentFileXferStatusMessage* status);
    bool g_key_get_string(char* data, const char* group, const char* key, char* value,
                                        unsigned vsize);
    bool g_key_get_uint64(char* data, const char* group, const char* key, uint64_t* value);

private:
    FileXferTasks _tasks;
};

#endif
