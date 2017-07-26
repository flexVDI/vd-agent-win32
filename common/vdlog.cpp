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

#include "config.h"
#include "vdcommon.h"
#include "vdlog.h"
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include <vector>

#define LOG_ROLL_SIZE (1024 * 1024)

VDLog* VDLog::_log = NULL;

VDLog::VDLog(FILE* handle)
    : _handle(handle)
{
    _log = this;
}

VDLog::~VDLog()
{
    if (_log && _handle) {
        fclose(_handle);
        _log = NULL;
    }
}

VDLog* VDLog::get(TCHAR* path)
{
    if (_log || !path) {
        return _log;
    }
    DWORD size = 0;
    HANDLE file = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             NULL);
    if (file != INVALID_HANDLE_VALUE) {
        size = GetFileSize(file, NULL);
        CloseHandle(file);
    }
    if (size != INVALID_FILE_SIZE && size > LOG_ROLL_SIZE) {
        TCHAR roll_path[MAX_PATH];
        swprintf_s(roll_path, MAX_PATH, L"%s.1", path);
        if (!MoveFileEx(path, roll_path, MOVEFILE_REPLACE_EXISTING)) {
            return NULL;
        }
    }
    FILE* handle = _wfsopen(path, L"a+", _SH_DENYNO);
    if (!handle) {
        return NULL;
    }
    _log = new VDLog(handle);
    return _log;
}

void VDLog::printf(const char* format, ...)
{
    FILE *fh = _log ? _log->_handle : stdout;
    va_list args;

    va_start(args, format);
    vfprintf(fh, format, args);
    va_end(args);
    fflush(fh);
}

void log_version()
{
    // print same version as resource one
    // RC_PRODUCTVERSION contains 4 numbers separated by commas (resource
    // version information source format).
    vd_printf("%u.%u.%u.%u", RC_PRODUCTVERSION);
}
