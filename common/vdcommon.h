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
#define VD_AGENT_STOP_EVENT   TEXT("Global\\vdagent_stop_event")

#if defined __GNUC__
#define ALIGN_GCC __attribute__ ((packed))
#define ALIGN_VC
#else
#define ALIGN_GCC
#define ALIGN_VC __declspec (align(1))
#endif

/*
 * Note: OLDMSVCRT, which is defined (in the Makefile) for mingw builds, and
 * is not defined for Visual Studio builds.
 *
 * On Windows XP some those functions are missing from the msvcrt.dll
 * When compiled with mingw, the program fails to run due to missing functions.
 * One can link to a newer runtime dll, e.g. msvcr100.dll, but that would
 * require installing that DLL on the guest. That can be done by downloading
 * and installing Microsoft Visual C++ 2010 Redistributable Package.
 * (same for 110.dll and 2012 Redistributable Package, etc).
 *
 * Since we do not want to add this dependency, we use functions that are
 * available in msvcrt.dll (and use define in the code).
 *
 * Currently Visual Studio builds are built with /MT (static mode) such that
 * those functions are not required to be in that dll on the guest.
 */
#ifdef OLDMSVCRT
#define swprintf_s(buf, sz, format...) swprintf(buf, format)

#ifndef _ftime_s
#define _ftime_s(timeb) _ftime(timeb)
#endif
#endif /* OLDMSVCRT */

#ifdef _MSC_VER // compiling with Visual Studio
#define snprintf         sprintf_s
#endif

enum SystemVersion {
    SYS_VER_UNSUPPORTED,
    SYS_VER_WIN_XP_CLASS, // also Server 2003/R2
    SYS_VER_WIN_7_CLASS,  // also Windows 8, Server 2012, Server 2008/R2 & Vista
};

int supported_system_version();

#endif

