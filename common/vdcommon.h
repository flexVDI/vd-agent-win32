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

#ifndef _H_VDCOMMON
#define _H_VDCOMMON

#pragma warning(disable:4200)

#include <windows.h>
#include "spice/vd_agent.h"
#include "vdlog.h"

typedef CRITICAL_SECTION mutex_t;

#define MUTEX_INIT(mutex) InitializeCriticalSection(&mutex)
#define MUTEX_LOCK(mutex) EnterCriticalSection(&mutex)
#define MUTEX_UNLOCK(mutex) LeaveCriticalSection(&mutex)

#define VD_SERVICE_PIPE_NAME   TEXT("\\\\.\\pipe\\vdservicepipe")
#define VD_MESSAGE_HEADER_SIZE (sizeof(VDPipeMessage) + sizeof(VDAgentMessage))
#define VD_PIPE_BUF_SIZE       (1024 * 1024)
#define VD_AGENT_REGISTRY_KEY "SOFTWARE\\Red Hat\\Spice\\vdagent\\"

enum {
    VD_AGENT_COMMAND,
    VD_AGENT_RESET,
    VD_AGENT_RESET_ACK,
    VD_AGENT_QUIT,
    VD_AGENT_SESSION_LOGON,
};

typedef __declspec (align(1)) struct VDPipeMessage {
    uint32_t type;
    uint32_t opaque;
    uint32_t size;
    uint8_t data[0];
} VDPipeMessage;

typedef struct VDPipeBuffer {
    OVERLAPPED overlap;
    DWORD start;
    DWORD end;
    uint8_t data[VD_PIPE_BUF_SIZE];
} VDPipeBuffer;

typedef struct VDPipeState {
    HANDLE pipe;
    VDPipeBuffer write;
    VDPipeBuffer read;
} VDPipeState;

#endif

