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

#if !defined __GNUC__
#pragma warning(disable:4200)
#endif

#include <windows.h>
#include "spice/vd_agent.h"
#include "vdlog.h"

typedef CRITICAL_SECTION mutex_t;

#define MUTEX_INIT(mutex) InitializeCriticalSection(&mutex)
#define MUTEX_LOCK(mutex) EnterCriticalSection(&mutex)
#define MUTEX_UNLOCK(mutex) LeaveCriticalSection(&mutex)

#define VD_AGENT_REGISTRY_KEY "SOFTWARE\\Red Hat\\Spice\\vdagent\\"

#if defined __GNUC__
#define ALIGN_GCC __attribute__ ((packed))
#define ALIGN_VC
#else
#define ALIGN_GCC
#define ALIGN_VC __declspec (align(1))
#endif

#ifdef OLDMSVCRT
#define swprintf_s(buf, sz, format...) swprintf(buf, format)
#endif

#ifdef OLDMSVCRT
#define _ftime_s(timeb) _ftime(timeb)
#endif

#endif

