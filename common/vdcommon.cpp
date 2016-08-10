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

#include <stdarg.h>

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

#ifndef HAVE_STRCAT_S
errno_t vdagent_strcat_s(char *strDestination,
                         size_t numberOfElements,
                         const char *strSource)
{
    if (strDestination == NULL)
        return EINVAL;
    if (strSource == NULL) {
        strDestination[0] = '\0';
        return EINVAL;
    }
    if (strlen(strDestination) + strlen(strSource) + 1 > numberOfElements) {
        strDestination[0] = '\0';
        return ERANGE;
    }

    strcat(strDestination, strSource);

    return 0;
}
#endif

#ifndef HAVE_STRCPY_S
errno_t vdagent_strcpy_s(char *strDestination,
                         size_t numberOfElements,
                         const char *strSource)
{
    if (strDestination == NULL)
        return EINVAL;
    strDestination[0] = '\0';
    if (strSource == NULL)
        return EINVAL;
    if (strlen(strSource) + 1 > numberOfElements) {
        return ERANGE;
    }

    strcpy(strDestination, strSource);

    return 0;
}
#endif

#ifndef HAVE_SWPRINTF_S
int vdagent_swprintf_s(wchar_t *buf, size_t len, const wchar_t *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int res = _vsnwprintf(buf, len, format, ap);
    va_end(ap);
    if ((res < 0 || (unsigned) res >= len) && len > 0) {
        buf[0] = 0;
    }
    return res;
}
#endif
