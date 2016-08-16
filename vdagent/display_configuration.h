/*
Copyright (C) 2015-2016 Red Hat, Inc.

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
#ifndef _H_DISPLAY_CONFIGURATION
#define _H_DISPLAY_CONFIGURATION

#include <spice/qxl_windows.h>
#include <spice/qxl_dev.h>
#include "desktop_layout.h"
#include "vdlog.h"

enum MONITOR_STATE {
    MONITOR_DETACHED,
    MONITOR_ATTACHED,
};

enum PATH_STATE {
    PATH_UPDATED,
    PATH_CURRENT,
};

enum DISPLAYCONFIG_TOPOLOGY_ID {};

struct DISPLAYCONFIG_DEVICE_INFO_HEADER;
struct DISPLAYCONFIG_MODE_INFO;
struct DISPLAYCONFIG_PATH_INFO;

//Makes calls into the CCD API for getting/setting display settings on WDDM drivers
//Use is exclusive to wddm display config class

typedef LONG(APIENTRY* PDISPLAYCONFIG_GETDEVICEINFO)(DISPLAYCONFIG_DEVICE_INFO_HEADER*);
typedef LONG(APIENTRY* PGETDISPLAYCONFIG_BUFFERSIZES)(UINT32, UINT32*, UINT32*);
typedef LONG(APIENTRY* PQUERYDISPLAYCONFIG)(UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO*, UINT32*,
                                            DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*);
typedef LONG(APIENTRY* PSETDISPLAYCONFIG)(UINT32, DISPLAYCONFIG_PATH_INFO*, UINT32,
                                          DISPLAYCONFIG_MODE_INFO*, UINT32);

class CCD {
public:
    CCD();
    ~CCD();

    bool query_display_config();
    bool set_display_config(LONG & error);
    bool update_mode_position(LPCTSTR device_name, DEVMODE* dev_mode);
    bool update_mode_size(LPCTSTR DeviceNmae, DEVMODE* dev_mode);
    void update_detached_primary_state(LPCTSTR device_name, DISPLAYCONFIG_PATH_INFO * path_info);
    bool set_path_state(LPCTSTR device_name, MONITOR_STATE state);
    bool is_attached(LPCTSTR device_name);
    bool is_active_path(DISPLAYCONFIG_PATH_INFO * path);
    DISPLAYCONFIG_MODE_INFO* get_active_mode(LPCTSTR device_name, bool return_on_error);
    DISPLAYCONFIG_PATH_INFO* get_device_path(LPCTSTR device_name, bool bActive);
    void verify_primary_position();
    void debug_print_config(const char* prefix = NULL);

private:
    bool load_api();
    bool get_config_buffers();
    void free_config_buffers();
    bool is_device_path(LPCTSTR device_name, DISPLAYCONFIG_PATH_INFO* path);
    bool get_device_name_config(DISPLAYCONFIG_PATH_INFO* path, LPTSTR dev_name);

    //CCD API stuff
    UINT32 _numPathElements;
    UINT32 _numModeElements;
    DISPLAYCONFIG_PATH_INFO* _pPathInfo;
    DISPLAYCONFIG_MODE_INFO* _pModeInfo;

    //CCD API function pointers
    PDISPLAYCONFIG_GETDEVICEINFO _pfnGetDeviceInfo;
    PGETDISPLAYCONFIG_BUFFERSIZES _pfnGetDisplayConfigBufferSizes;
    PQUERYDISPLAYCONFIG _pfnQueryDisplayConfig;
    PSETDISPLAYCONFIG _pfnSetDisplayConfig;

    bool  _primary_detached;
    PATH_STATE _path_state;
};

class DisplayMode;

//Class provides interface to get/set display configurations
class DisplayConfig {
public:
    static DisplayConfig* create_config();
    DisplayConfig();
    virtual ~DisplayConfig() {};
    virtual bool is_attached(DISPLAY_DEVICE* dev_info) = 0;
    virtual bool custom_display_escape(LPCTSTR device, DEVMODE* mode) = 0;
    virtual bool update_monitor_config(LPCTSTR device, DisplayMode* mode, DEVMODE* dev_mode) = 0;
    virtual bool set_monitor_state(LPCTSTR device_name, DEVMODE* dev_mode, MONITOR_STATE state) = 0;
    virtual LONG update_display_settings() = 0;
    virtual bool update_dev_mode_position(LPCTSTR dev_name, DEVMODE* dev_mode, LONG x, LONG y) = 0;
    void set_monitors_config(bool flag) { _send_monitors_config = flag; }
    virtual void update_config_path() {};

protected:
    bool _send_monitors_config;
};

//DisplayConfig implementation for guest with XPDM graphics drivers
class XPDMInterface : public DisplayConfig {
public:
    XPDMInterface() :DisplayConfig() {};
    bool is_attached(DISPLAY_DEVICE* dev_info);
    bool custom_display_escape(LPCTSTR device_name, DEVMODE* dev_mode);
    bool update_monitor_config(LPCTSTR device_name, DisplayMode* mode, DEVMODE* dev_mode);
    bool set_monitor_state(LPCTSTR device_name, DEVMODE* dev_mode, MONITOR_STATE state);
    LONG update_display_settings();
    bool update_dev_mode_position(LPCTSTR device_name, DEVMODE * dev_mode, LONG x, LONG y);

private:
    bool find_best_mode(LPCTSTR Device, DEVMODE* dev_mode);
};

#endif