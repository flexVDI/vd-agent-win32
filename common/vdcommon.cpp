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

#include "vdcommon.h"

int supported_system_version()
{
    OSVERSIONINFOEX osvi;

    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (!GetVersionEx((OSVERSIONINFO*)&osvi)) {
        vd_printf("GetVersionEx() failed: %lu", GetLastError());
        return 0;
    }
    if (osvi.dwMajorVersion == 5 && (osvi.dwMinorVersion == 1 || osvi.dwMinorVersion == 2)) {
        return SYS_VER_WIN_XP_CLASS;
    } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion >= 0 && osvi.dwMinorVersion <= 2) {
        return SYS_VER_WIN_7_CLASS;
    }
    return 0;
}
