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

#ifndef _H_VDLOG
#define _H_VDLOG

#include <tchar.h>
#include <crtdbg.h>
#include <windows.h>

#define LOG_ENABLED

class VDLog {
public:
    ~VDLog();
    static VDLog* get(TCHAR* path = NULL);
    void printf(const char* format, ...);

private:
    VDLog(FILE* handle);

private:
    static VDLog* _log;
    FILE* _handle;
};

#ifdef LOG_ENABLED
#define vd_printf(format, ...) {                                                    \
    VDLog* log = VDLog::get();                                                      \
    double secs = GetTickCount() / 1000.0;                                          \
    if (log) {                                                                      \
        log->printf("%.3f %s: " format "\n", secs, __FUNCTION__, __VA_ARGS__);      \
    } else {                                                                        \
        printf("%.3f %s: " format "\n", secs, __FUNCTION__, __VA_ARGS__);           \
    }                                                                               \
}

#define ASSERT(x) _ASSERTE(x)
#else
#define vd_printf(format, ...)
#define ASSERT(x)
#endif

void log_version();

#endif
