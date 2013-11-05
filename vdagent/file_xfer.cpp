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

#include <shlobj.h>
#define __STDC_FORMAT_MACROS
#define __USE_MINGW_ANSI_STDIO 1
#include <inttypes.h>
#include <stdio.h>
#include "file_xfer.h"
#include "as_user.h"

FileXfer::~FileXfer()
{
    FileXferTasks::iterator iter;
    FileXferTask* task;

    for (iter = _tasks.begin(); iter != _tasks.end(); iter++) {
        task = iter->second;
        CloseHandle(task->handle);
        DeleteFileA(task->name);
        delete task;
    }
}

void FileXfer::handle_start(VDAgentFileXferStartMessage* start,
                            VDAgentFileXferStatusMessage* status)
{
    char* file_meta = (char*)start->data;
    char file_path[MAX_PATH], file_name[MAX_PATH];
    ULARGE_INTEGER free_bytes;
    FileXferTask* task;
    uint64_t file_size;
    HANDLE handle;
    AsUser as_user;

    status->id = start->id;
    status->result = VD_AGENT_FILE_XFER_STATUS_ERROR;
    if (!g_key_get_string(file_meta, "vdagent-file-xfer", "name", file_name) ||
            !g_key_get_uint64(file_meta, "vdagent-file-xfer", "size", &file_size)) {
        vd_printf("file id %u meta parsing failed", start->id);
        return;
    }
    vd_printf("%u %s (%" PRIu64 ")", start->id, file_name, file_size);
    if (!as_user.begin()) {
        vd_printf("as_user failed");
        return;
    }

    if (FAILED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE, NULL,
            SHGFP_TYPE_CURRENT, file_path))) {
        vd_printf("failed getting desktop path");
        return;
    }
    if (!GetDiskFreeSpaceExA(file_path, &free_bytes, NULL, NULL)) {
        vd_printf("failed getting disk free space %lu", GetLastError());
        return;
    }
    if (free_bytes.QuadPart < file_size) {
        vd_printf("insufficient disk space %" PRIu64, free_bytes.QuadPart);
        return;
    }

    if (strlen(file_path) + strlen(file_name) + 1 >= MAX_PATH) {
        vd_printf("error: file too long %s\%s", file_path, file_name);
        return;
    }

    strcat(file_path, "\\");
    strcat(file_path, file_name);
    handle = CreateFileA(file_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        vd_printf("failed creating %s %lu", file_path, GetLastError());
        return;
    }
    task = new FileXferTask(handle, file_size, file_path);
    _tasks[start->id] = task;
    status->result = VD_AGENT_FILE_XFER_STATUS_CAN_SEND_DATA;
}

bool FileXfer::handle_data(VDAgentFileXferDataMessage* data,
                           VDAgentFileXferStatusMessage* status)
{
    FileXferTasks::iterator iter;
    FileXferTask* task = NULL;
    DWORD written;

    status->id = data->id;
    status->result = VD_AGENT_FILE_XFER_STATUS_ERROR;
    iter = _tasks.find(data->id);
    if (iter == _tasks.end()) {
        vd_printf("file id %u not found", data->id);
        goto fin;
    }
    task = iter->second;
    task->pos += data->size;
    if (task->pos > task->size) {
        vd_printf("file xfer is longer than expected");
        goto fin;
    }  
    if (!WriteFile(task->handle, data->data, (DWORD)data->size,
                   &written, NULL) || written != data->size) {
        vd_printf("file write failed %lu", GetLastError());
        goto fin;
    }
    if (task->pos < task->size) {
        return false;
    }
    vd_printf("%u completed", iter->first);
    status->result = VD_AGENT_FILE_XFER_STATUS_SUCCESS;

fin:
    if (task) {
        CloseHandle(task->handle);
        if (status->result != VD_AGENT_FILE_XFER_STATUS_SUCCESS) {
            DeleteFileA(task->name);
        }
        _tasks.erase(iter);
        delete task;
    }

    return true;
}

void FileXfer::handle_status(VDAgentFileXferStatusMessage* status)
{
    FileXferTasks::iterator iter;
    FileXferTask* task;

    vd_printf("id %u result %u", status->id, status->result); 
    if (status->result != VD_AGENT_FILE_XFER_STATUS_CANCELLED) {
        vd_printf("only cancel is premitted");
        return;
    }
    iter = _tasks.find(status->id);
    if (iter == _tasks.end()) {
        vd_printf("file id %u not found", status->id);
        return;
    }
    task = iter->second;
    CloseHandle(task->handle);
    DeleteFileA(task->name);
    _tasks.erase(iter);
    delete task;
}

bool FileXfer::dispatch(VDAgentMessage* msg, VDAgentFileXferStatusMessage* status)
{
    bool ret = false;

    switch (msg->type) {
    case VD_AGENT_FILE_XFER_START:
        handle_start((VDAgentFileXferStartMessage*)msg->data, status);
        ret = true;
        break;
    case VD_AGENT_FILE_XFER_DATA:
        ret = handle_data((VDAgentFileXferDataMessage*)msg->data, status);
        break;
    case VD_AGENT_FILE_XFER_STATUS:
        handle_status((VDAgentFileXferStatusMessage*)msg->data);
        break;
    default:
        vd_printf("unsupported message type %u size %u", msg->type, msg->size);
    }
    return ret;
}

//minimal parsers for GKeyFile, supporting only key=value with no spaces.
#define G_KEY_MAX_LEN 256

bool FileXfer::g_key_get_string(char* data, const char* group, const char* key, char* value)
{
    char group_pfx[G_KEY_MAX_LEN], key_pfx[G_KEY_MAX_LEN];
    char *group_pos, *key_pos, *next_group_pos;

    snprintf(group_pfx, sizeof(group_pfx), "[%s]", group);
    if (!(group_pos = strstr((char*)data, group_pfx))) return false;

    snprintf(key_pfx, sizeof(key_pfx), "\n%s=", key);
    if (!(key_pos = strstr(group_pos, key_pfx))) return false;

    next_group_pos = strstr(group_pos + strlen(group_pfx), "[");
    if (next_group_pos && key_pos > next_group_pos) return false; 

    return !!sscanf(key_pos + strlen(key_pfx), "%s\n", value);
}

bool FileXfer::g_key_get_uint64(char* data, const char* group, const char* key, uint64_t* value)
{
    char str[G_KEY_MAX_LEN];

    if (!g_key_get_string(data, group, key, str)) return false;
    return !!sscanf(str, "%" PRIu64, value);
}
