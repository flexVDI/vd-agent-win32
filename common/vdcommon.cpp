#include "vdcommon.h"

bool get_qxl_device_id(TCHAR* device_key, DWORD* device_id)
{
    DWORD type = REG_BINARY;
    DWORD size = sizeof(*device_id);
    bool key_found = false;
    HKEY key;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, wcsstr(device_key, L"System"),
                     0L, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueEx(key, L"QxlDeviceID", NULL, &type, (LPBYTE)device_id, &size) ==
                                                                             ERROR_SUCCESS) {
            key_found = true;
        }
        RegCloseKey(key);
    }
    return key_found;
}
