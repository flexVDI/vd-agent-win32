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
#include "display_configuration.h"
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
    , _display_config (NULL)
{
    _display_config = DisplayConfig::create_config();
    get_displays();
}

DesktopLayout::~DesktopLayout()
{
    clean_displays();
    delete _display_config;
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
    _display_config->update_config_path();
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
        attached = _display_config->is_attached(&dev_info);

        EnumDisplaySettings(dev_info.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
        _displays[display_id] = new DisplayMode(mode.dmPosition.x, mode.dmPosition.y,
                                                mode.dmPelsWidth, mode.dmPelsHeight,
                                                mode.dmBitsPerPel, attached);
        _display_config->update_monitor_config(dev_info.DeviceName, _displays[display_id], &mode);
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
    _display_config->update_config_path();
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
            vd_printf("display_id %lu out of range, #displays %lu",
                      display_id, (unsigned long) _displays.size());
            break;
        }
        DisplayMode * mode(_displays.at(display_id));
        if (!init_dev_mode(dev_info.DeviceName, &dev_mode, mode)) {
            vd_printf("No suitable mode found for display %S", dev_info.DeviceName);
            break;
        }
        vd_printf("Set display mode %lux%lu", dev_mode.dmPelsWidth, dev_mode.dmPelsHeight);
        if (_display_config->update_dev_mode_position(dev_info.DeviceName, &dev_mode,
                                                     mode->_pos_x - normal_x,
                                                     mode->_pos_y - normal_y)) {
            dev_sets++;
            _display_config->update_monitor_config(dev_info.DeviceName, mode, &dev_mode);
        }
        if (!is_qxl) {
            display_id++;
        }
    }
    if (dev_sets) {
        _display_config->update_display_settings();
        normalize_displays_pos();
    }
    unlock();
}

void DesktopLayout::set_position_configurable(bool flag) {
    _display_config->set_monitors_config(flag);
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

bool DesktopLayout::init_dev_mode(LPCTSTR dev_name, DEVMODE* dev_mode, DisplayMode* mode)
{
    ZeroMemory(dev_mode, sizeof(DEVMODE));
    dev_mode->dmSize = sizeof(DEVMODE);

    //Update monitor state
    MONITOR_STATE monitor_state = (!mode || !mode->_attached)? MONITOR_DETACHED : MONITOR_ATTACHED;
    _display_config->set_monitor_state(dev_name, dev_mode, monitor_state);
    if (monitor_state == MONITOR_DETACHED) {
        return true;
    }

    // Update custom resolution
    dev_mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    dev_mode->dmPelsWidth = mode->_width;
    dev_mode->dmPelsHeight = mode->_height;
    dev_mode->dmBitsPerPel = mode->_depth;
    if (!_display_config->custom_display_escape(dev_name, dev_mode))
        return false;

    // update current DisplayMode (so mouse scaling works properly)
    mode->_width = dev_mode->dmPelsWidth;
    mode->_height = dev_mode->dmPelsHeight;
    return true;
}
