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
#include "as_user.h"

#include <wtsapi32.h>

AsUser::AsUser(DWORD session_id):
    _session_id(session_id),
    _token(INVALID_HANDLE_VALUE),
    _started(false)
{
}

bool AsUser::begin()
{
    BOOL ret;

    if (_session_id == (DWORD)-1) {
        ret = ProcessIdToSessionId(GetCurrentProcessId(), &_session_id);
        if (!ret) {
            vd_printf("ProcessIdToSessionId failed %lu", GetLastError());
            return false;
        }
    }
    if (_token == INVALID_HANDLE_VALUE) {
        ret = WTSQueryUserToken(_session_id, &_token);
        if (!ret) {
            vd_printf("WTSQueryUserToken failed -- %lu", GetLastError());
        return false;
        }
    }

    ret = ImpersonateLoggedOnUser(_token);
    if (!ret) {
        vd_printf("ImpersonateLoggedOnUser failed: %lu", GetLastError());
        return false;
    }

    _started = true;
    return true;
}

void AsUser::end()
{
    if (_started) {
        RevertToSelf();
        _started = false;
    }
}

AsUser::~AsUser()
{
    end();
    if (_token != INVALID_HANDLE_VALUE) {
        CloseHandle(_token);
    }
}
