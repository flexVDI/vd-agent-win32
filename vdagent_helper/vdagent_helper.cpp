#include <windows.h>
#include <stdio.h>
#include <vdlog.h>

int main(int argc,char **argv)
{
    TCHAR log_path[MAX_PATH];
    HANDLE pipe;
    INPUT input;
    HDESK hdesk;
    DWORD bytes;
    DWORD err = 0;
    VDLog* log;

    if (GetTempPath(MAX_PATH, log_path)) {
        wcscat(log_path, L"vdagent_helper.log");
        log = VDLog::get(log_path);
    }
    vd_printf("***vdagent_helper started***");
    pipe = CreateFile(VD_AGENT_NAMED_PIPE, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        vd_printf("Cannot open pipe %S: %lu", VD_AGENT_NAMED_PIPE, GetLastError());
        goto fin;
    }
    while (ReadFile(pipe, &input, sizeof(input), &bytes, NULL) && bytes == sizeof(input)) {
        hdesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!hdesk) {
            vd_printf("OpenInputDesktop() failed: %lu", GetLastError());
            break;
        }
        if (!SetThreadDesktop(hdesk)) {
            vd_printf("SetThreadDesktop() failed: %lu", GetLastError());
            CloseDesktop(hdesk);
            break;
        }
        if (!SendInput(1, &input, sizeof(input)) && err != GetLastError()) {
            err = GetLastError();
            vd_printf("SendInput() failed: %lu", err);
        }
        CloseDesktop(hdesk);
    }
    CloseHandle(pipe);
fin:
    delete log;
    return 0;
}