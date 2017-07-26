/*
   Copyright (C) 2017 Red Hat, Inc.

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
#undef NDEBUG
#include <assert.h>
#include "vdlog.h"

using namespace std;

int main(int argc, char **argv)
{
    TCHAR temp_path[MAX_PATH];
    assert(GetTempPath(ARRAYSIZE(temp_path), temp_path) != 0);

    TCHAR path[MAX_PATH];
    assert(GetTempFileName(temp_path, TEXT("tst"), 0, path) != 0);

    VDLog *log = VDLog::get(path);
    assert(log);

    log_version();
    vd_printf("Log something");
    log->printf("A number %d", 123456);
    delete log;

    FILE *f = _wfopen(path, L"r");
    assert(f);
    char line[1024];
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "log_version") != NULL);
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "Log something") != NULL);
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "A number 123456") != NULL);
    fclose(f);

    DeleteFile(path);
    return 0;
}
