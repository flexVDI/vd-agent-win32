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

#include "desktop_layout.h"
#include "vdlog.h"

void DisplayMode::set_res(DWORD width, DWORD height, DWORD depth)
{
    _width = width;
    _height = height;
    _depth = depth;
}

DesktopLayout::DesktopLayout()
    : _total_width (0)
    , _total_height (0)
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
    DWORD qxl_id;
    DWORD dev_id = 0;
    LONG min_x = 0;
    LONG min_y = 0;
    LONG max_x = 0;
    LONG max_y = 0;
    bool attached;

    lock();
    clean_displays();
    ZeroMemory(&dev_info, sizeof(dev_info));
    dev_info.cb = sizeof(dev_info);
    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = sizeof(mode);
    while (EnumDisplayDevices(NULL, dev_id, &dev_info, 0)) {
        if (wcsstr(dev_info.DeviceString, L"QXL")) {
            attached = !!(dev_info.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
            EnumDisplaySettings(dev_info.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
            if (!get_qxl_device_id(dev_info.DeviceKey, &qxl_id)) {
                vd_printf("get_qxl_device_id failed");
                break;
            }
            size_t size = _displays.size();
            if (qxl_id >= size) {
                _displays.resize(qxl_id + 1);
                for (size_t i = size; i < qxl_id; i++) {
                    _displays[i] = NULL;
                }
            }
            _displays[qxl_id] = new DisplayMode(mode.dmPosition.x, mode.dmPosition.y,
                                                mode.dmPelsWidth, mode.dmPelsHeight,
                                                mode.dmBitsPerPel, attached);
            if (attached) {
                min_x = min(min_x, mode.dmPosition.x);
                min_y = min(min_y, mode.dmPosition.y);
                max_x = max(max_x, mode.dmPosition.x + (LONG)mode.dmPelsWidth);
                max_y = max(max_y, mode.dmPosition.y + (LONG)mode.dmPelsHeight);
            }
        }
        dev_id++;
    }
    if (min_x || min_y) {
        for (Displays::iterator iter = _displays.begin(); iter != _displays.end(); iter++) {
            (*iter)->move_pos(-min_x, -min_y);
        }
    }
    _total_width = max_x - min_x;
    _total_height = max_y - min_y;
    unlock();
}

void DesktopLayout::set_displays()
{
    DISPLAY_DEVICE dev_info;
    DEVMODE dev_mode;
    DWORD dev_id = 0;
    DWORD qxl_id = 0;
    int dev_sets = 0;

    lock();
    ZeroMemory(&dev_info, sizeof(dev_info));
    dev_info.cb = sizeof(dev_info);
    ZeroMemory(&dev_mode, sizeof(dev_mode));
    dev_mode.dmSize = sizeof(dev_mode);
    while (EnumDisplayDevices(NULL, dev_id, &dev_info, 0)) {
        if (wcsstr(dev_info.DeviceString, L"QXL")) {
            if (!get_qxl_device_id(dev_info.DeviceKey, &qxl_id)) {
                vd_printf("get_qxl_device_id failed");
                break;
            }
            if (qxl_id >= _displays.size()) {
                vd_printf("qxl_id %u out of range, #displays %u", qxl_id, _displays.size());
                break;
            }
            //FIXME: always set pos?
            init_dev_mode(&dev_mode, _displays.at(qxl_id), true);
            LONG ret = ChangeDisplaySettingsEx(dev_info.DeviceName, &dev_mode, NULL,
                                               CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
            if (ret == DISP_CHANGE_SUCCESSFUL) {
                dev_sets++;
            }
        }
        dev_id++;
    }
    if (dev_sets) {
        ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL);
    }
    unlock();
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

void DesktopLayout::init_dev_mode(DEVMODE* dev_mode, DisplayMode* mode, bool set_pos)
{
    ZeroMemory(dev_mode, sizeof(DEVMODE));
    dev_mode->dmSize = sizeof(DEVMODE);
    if (mode && mode->get_attached()) {
        dev_mode->dmBitsPerPel = mode->get_depth();
        dev_mode->dmPelsWidth = mode->get_width();
        dev_mode->dmPelsHeight = mode->get_height();
        dev_mode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
        if (set_pos) {
            dev_mode->dmPosition.x = mode->get_pos_x();
            dev_mode->dmPosition.y = mode->get_pos_y();
            dev_mode->dmFields |= DM_POSITION;
        }
    } else {
        //detach monitor
        dev_mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    }
}

