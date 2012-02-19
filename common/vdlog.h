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

#include <stdio.h>
#include <tchar.h>
#include <crtdbg.h>
#include <windows.h>
#include <time.h>
#include <sys/timeb.h>

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

enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
  LOG_FATAL
};

#ifdef _DEBUG
static unsigned int log_level = LOG_DEBUG;
#else
static unsigned int log_level = LOG_INFO;
#endif

#define PRINT_LINE(type, format, datetime, ms, ...)                                             \
    printf("%lu::%s::%s,%.3d::%s::" format "\n", GetCurrentThreadId(), type, datetime, ms,       \
           __FUNCTION__, ## __VA_ARGS__);

#define LOG(type, format, ...) if (type >= log_level && type <= LOG_FATAL) {                    \
    VDLog* log = VDLog::get();                                                                  \
    const char *type_as_char[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };                 \
    struct _timeb now;                                                                          \
    struct tm today;                                                                            \
    char datetime_str[20];                                                                      \
    _ftime_s(&now);                                                                             \
    localtime_s(&today, &now.time);                                                             \
    strftime(datetime_str, 20, "%Y-%m-%d %H:%M:%S", &today);                                    \
    if (log) {                                                                                  \
        log->PRINT_LINE(type_as_char[type], format, datetime_str, now.millitm, ## __VA_ARGS__); \
    } else {                                                                                    \
        PRINT_LINE(type_as_char[type], format, datetime_str, now.millitm, ## __VA_ARGS__);      \
    }                                                                                           \
}
 
#define vd_printf(format, ...) LOG(LOG_INFO, format, ## __VA_ARGS__)
#define LOG_INFO(format, ...) LOG(LOG_INFO, format, ## __VA_ARGS__)
#define LOG_WARN(format, ...) LOG(LOG_WARN, format, ## __VA_ARGS__)
#define LOG_ERROR(format, ...) LOG(LOG_ERROR, format, ## __VA_ARGS__)

#define DBGLEVEL 1000

#define DBG(level, format, ...) {               \
    if (level <= DBGLEVEL) {                    \
        LOG(LOG_DEBUG, format, ## __VA_ARGS__); \
    }                                           \
}

#define ASSERT(x) _ASSERTE(x)

void log_version();

#endif
