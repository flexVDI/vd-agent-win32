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

#include <spice/qxl_windows.h>
#include <spice/qxl_dev.h>
#include "desktop_layout.h"
#include "vdlog.h"

#ifdef __MINGW32__
using std::min;
using std::max;
#endif

void DisplayMode::set_res(DWORD width, DWORD height, DWORD depth)
{
    _width = width;
    _height = height;
    _depth = depth;
}

DesktopLayout::DesktopLayout()
    : _total_width (0)
    , _total_height (0)
    , _send_monitors_position(false)
{
    MUTEX_INIT(_mutex);
    get_displays();
}

DesktopLayout::~DesktopLayout()
{
    clean_displays();
}

void DesktopLayout::get_displays()
{
    DISPLAY_DEVICE dev_info;
    DEVMODE mode;
    DWORD display_id;
    DWORD dev_id = 0;
    bool attached;

    lock();
    if (!consistent_displays()) {
        unlock();
        return;
    }
    clean_displays();
    ZeroMemory(&dev_info, sizeof(dev_info));
    dev_info.cb = sizeof(dev_info);
    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = sizeof(mode);
    while (EnumDisplayDevices(NULL, dev_id, &dev_info, 0)) {
        dev_id++;
        if (dev_info.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
            continue;
        }
        size_t size = _displays.size();
        if (!wcsstr(dev_info.DeviceString, L"QXL")) {
            display_id = (DWORD)size;
        } else if (!get_qxl_device_id(dev_info.DeviceKey, &display_id)) {
            vd_printf("get_qxl_device_id failed %S", dev_info.DeviceKey);
            break;
        }
        if (display_id >= size) {
            _displays.resize(display_id + 1);
            for (size_t i = size; i < display_id; i++) {
                _displays[i] = NULL;
            }
        }
        attached = !!(dev_info.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
        EnumDisplaySettings(dev_info.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
        _displays[display_id] = new DisplayMode(mode.dmPosition.x, mode.dmPosition.y,
                                                mode.dmPelsWidth, mode.dmPelsHeight,
                                                mode.dmBitsPerPel, attached);
        update_monitor_config(dev_info.DeviceName, _displays[display_id]);
    }
    normalize_displays_pos();
    unlock();
}

DisplayMode * DesktopLayout::get_primary_display()
{
    DisplayMode * mode;

    for (unsigned int i = 0; i < get_display_count(); i++)
    {
        mode = _displays.at(i);
        if (!mode)
            continue;
        if (mode->is_primary())
            return mode;
    }
    return NULL;
}

void DesktopLayout::set_displays()
{
    DISPLAY_DEVICE dev_info;
    DEVMODE dev_mode;
    DWORD dev_id = 0;
    DWORD display_id = 0;
    int dev_sets = 0;

    lock();
    if (!consistent_displays()) {
        unlock();
        return;
    }
    ZeroMemory(&dev_info, sizeof(dev_info));
    dev_info.cb = sizeof(dev_info);
    ZeroMemory(&dev_mode, sizeof(dev_mode));
    dev_mode.dmSize = sizeof(dev_mode);

    //Get the normalized position of the primary monitor
    DisplayMode * primary(get_primary_display());
    LONG normal_x = primary ? primary->get_pos_x() : 0;
    LONG normal_y = primary ? primary->get_pos_y() : 0;

    while (EnumDisplayDevices(NULL, dev_id, &dev_info, 0)) {
        dev_id++;
        if (dev_info.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
            continue;
        }
        bool is_qxl = !!wcsstr(dev_info.DeviceString, L"QXL");
        if (is_qxl && !get_qxl_device_id(dev_info.DeviceKey, &display_id)) {
            vd_printf("get_qxl_device_id failed %S", dev_info.DeviceKey);
            break;
        }
        if (display_id >= _displays.size()) {
            vd_printf("display_id %lu out of range, #displays %zu" , display_id, _displays.size());
            break;
        }
        DisplayMode * mode(_displays.at(display_id));
        if (!init_dev_mode(dev_info.DeviceName, &dev_mode, mode, normal_x, normal_y, true)) {
            vd_printf("No suitable mode found for display %S", dev_info.DeviceName);
            break;
        }
        vd_printf("Set display mode %lux%lu", dev_mode.dmPelsWidth, dev_mode.dmPelsHeight);
        LONG ret = ChangeDisplaySettingsEx(dev_info.DeviceName, &dev_mode, NULL,
                                           CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
        if (ret == DISP_CHANGE_SUCCESSFUL) {
            dev_sets++;
            update_monitor_config(dev_info.DeviceName, mode);
        }
        if (!is_qxl) {
            display_id++;
        }
    }
    if (dev_sets) {
        ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL);
        normalize_displays_pos();
    }
    unlock();
}

// Normalize all display positions to non-negative coordinates and update total width and height of
// the virtual desktop. Caller is responsible to lock() & unlock().
void DesktopLayout::normalize_displays_pos()
{
    Displays::iterator iter;
    DisplayMode* mode;
    LONG min_x = 0;
    LONG min_y = 0;
    LONG max_x = 0;
    LONG max_y = 0;

    for (iter = _displays.begin(); iter != _displays.end(); iter++) {
        mode = *iter;
        if (mode && mode->_attached) {
            min_x = min(min_x, mode->_pos_x);
            min_y = min(min_y, mode->_pos_y);
            max_x = max(max_x, mode->_pos_x + (LONG)mode->_width);
            max_y = max(max_y, mode->_pos_y + (LONG)mode->_height);
        }
    }
    if (min_x || min_y) {
        for (iter = _displays.begin(); iter != _displays.end(); iter++) {
            mode = *iter;
            if (mode) {
                mode->move_pos(-min_x, -min_y);
            }
        }
    }
    _total_width = max_x - min_x;
    _total_height = max_y - min_y;
}

bool DesktopLayout::consistent_displays()
{
    DISPLAY_DEVICE dev_info;
    DWORD dev_id = 0;
    int non_qxl_count = 0;
    int qxl_count = 0;

    ZeroMemory(&dev_info, sizeof(dev_info));
    dev_info.cb = sizeof(dev_info);
    while (EnumDisplayDevices(NULL, dev_id, &dev_info, 0)) {
        dev_id++;
        if (dev_info.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) {
            continue;
        }
        if (wcsstr(dev_info.DeviceString, L"QXL")) {
            qxl_count++;
        } else {
            non_qxl_count++;
        }
    }
    vd_printf("#qxls %d #others %d", qxl_count, non_qxl_count);
    return (!qxl_count || !non_qxl_count);
}

void DesktopLayout::clean_displays()
{
    lock();
    _total_width = 0;
    _total_height = 0;
    while (!_displays.empty()) {
        DisplayMode* mode = _displays.back();
        _displays.pop_back();
        delete mode;
    }
    unlock();
}

bool DesktopLayout::is_attached(LPCTSTR dev_name)
{
    DEVMODE dev_mode;

    ZeroMemory(&dev_mode, sizeof(dev_mode));
    dev_mode.dmSize = sizeof(dev_mode);
    EnumDisplaySettings(dev_name, ENUM_CURRENT_SETTINGS, &dev_mode);
    return !!dev_mode.dmBitsPerPel;
}

bool DesktopLayout::get_qxl_device_id(WCHAR* device_key, DWORD* device_id)
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

bool DesktopLayout::init_dev_mode(LPCTSTR dev_name, DEVMODE* dev_mode, DisplayMode* mode,
                                  LONG normal_x, LONG normal_y, bool set_pos)
{
    DWORD closest_diff = -1;
    DWORD best = -1;
    QXLEscapeSetCustomDisplay custom;
    HDC hdc = NULL;
    LONG ret;

    ZeroMemory(dev_mode, sizeof(DEVMODE));
    dev_mode->dmSize = sizeof(DEVMODE);
    if (!mode || !mode->_attached) {
        //Detach monitor
        dev_mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
        return true;
    }

    hdc = CreateDC(dev_name, NULL, NULL, NULL);
    if (!hdc) {
        // for some reason, windows want those 3 flags to enable monitor
        dev_mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
        dev_mode->dmPelsWidth = mode->_width;
        dev_mode->dmPelsHeight = mode->_height;
        ret = ChangeDisplaySettingsEx(dev_name, dev_mode, NULL, CDS_UPDATEREGISTRY, NULL);
        if (ret == DISP_CHANGE_BADMODE) {
            // custom resolution might not be set yet, use known resolution
            // FIXME: this causes client temporary resize... a
            // solution would involve passing custom resolution before
            // driver initialization, perhaps through registry
            dev_mode->dmPelsWidth = 640;
            dev_mode->dmPelsHeight = 480;
            ret = ChangeDisplaySettingsEx(dev_name, dev_mode, NULL, CDS_UPDATEREGISTRY, NULL);
        }

        vd_printf("attach %ld", ret);
        hdc = CreateDC(dev_name, NULL, NULL, NULL);
    }

    if (!hdc) {
        vd_printf("failed to create DC");
        return false;
    } else {
        // Update custom resolution
        custom.xres = mode->_width;
        custom.yres = mode->_height;
        custom.bpp = mode->_depth;

        int err = ExtEscape(hdc, QXL_ESCAPE_SET_CUSTOM_DISPLAY,
                            sizeof(QXLEscapeSetCustomDisplay), (LPCSTR)&custom, 0, NULL);
        if (err <= 0) {
            vd_printf("can't set custom display, perhaps an old driver");
        }
        DeleteDC(hdc);
    }

    // force refresh mode table
    DEVMODE tempDevMode;
    ZeroMemory(&tempDevMode, sizeof (tempDevMode));
    tempDevMode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(dev_name, 0xffffff, &tempDevMode);

    //Find the closest size which will fit within the monitor
    for (DWORD i = 0; EnumDisplaySettings(dev_name, i, dev_mode); i++) {
        if (dev_mode->dmPelsWidth > mode->_width ||
            dev_mode->dmPelsHeight > mode->_height ||
            dev_mode->dmBitsPerPel != mode->_depth) {
            continue;
        }
        DWORD wdiff = mode->_width - dev_mode->dmPelsWidth;
        DWORD hdiff = mode->_height - dev_mode->dmPelsHeight;
        DWORD diff = wdiff * wdiff + hdiff * hdiff;
        if (diff < closest_diff) {
            closest_diff = diff;
            best = i;
        }
    }
    if (best == (DWORD)-1 || !EnumDisplaySettings(dev_name, best, dev_mode)) {
        return false;
    }
    dev_mode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (set_pos) {
        //Convert the position so that the primary is always at (0,0)
        dev_mode->dmPosition.x = mode->_pos_x - normal_x;
        dev_mode->dmPosition.y = mode->_pos_y - normal_y;
        dev_mode->dmFields |= DM_POSITION;
    }

    // update current DisplayMode (so mouse scaling works properly)
    mode->_width = dev_mode->dmPelsWidth;
    mode->_height = dev_mode->dmPelsHeight;

    return true;
}

bool DesktopLayout::update_monitor_config(LPCTSTR dev_name, DisplayMode* mode)
{
    QXLHead monitor_config;

    if (!mode || !mode->get_attached())
        return false;

    //Don't configure monitors unless the client supports it
    if(!_send_monitors_position) return FALSE;

    HDC hdc = CreateDC(dev_name, NULL, NULL, NULL);

    memset(&monitor_config, 0, sizeof(monitor_config));
    monitor_config.x = mode->_pos_x;
    monitor_config.y = mode->_pos_y;
    monitor_config.width = mode->_width;
    monitor_config.height = mode->_height;

    int err = ExtEscape(hdc, QXL_ESCAPE_MONITOR_CONFIG,
        sizeof(QXLHead), (LPCSTR) &monitor_config, 0, NULL);

    if (err < 0){
        vd_printf("can't update monitor config, may have an older driver");
    }

    DeleteDC(hdc);
    return (err >= 0);
}
