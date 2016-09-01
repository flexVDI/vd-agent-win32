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

#include "display_configuration.h"
#include <winternl.h>
#include <spice/macros.h>

/* The following definitions and structures are taken
from the wine project repository and can be found
under: "wine/include/wingdi.h" */

#define QDC_ALL_PATHS                          0x00000001

#define SDC_USE_SUPPLIED_DISPLAY_CONFIG        0x00000020
#define SDC_APPLY                              0x00000080
#define SDC_SAVE_TO_DATABASE                   0x00000200
#define SDC_FORCE_MODE_ENUMERATION             0x00001000

#define DISPLAYCONFIG_PATH_ACTIVE              0x00000001
#define DISPLAYCONFIG_PATH_MODE_IDX_INVALID    0xffffffff

enum DISPLAYCONFIG_DEVICE_INFO_TYPE {
    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME = 1
};

enum DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY {};

enum DISPLAYCONFIG_ROTATION {};

enum DISPLAYCONFIG_SCANLINE_ORDERING {};

enum DISPLAYCONFIG_SCALING {};

enum DISPLAYCONFIG_PIXELFORMAT {};

enum DISPLAYCONFIG_MODE_INFO_TYPE {};

struct DISPLAYCONFIG_DEVICE_INFO_HEADER {
    DISPLAYCONFIG_DEVICE_INFO_TYPE type;
    UINT32                         size;
    LUID                           adapterId;
    UINT32                         id;
};

struct DISPLAYCONFIG_SOURCE_DEVICE_NAME {
    DISPLAYCONFIG_DEVICE_INFO_HEADER          header;
    WCHAR                                     viewGdiDeviceName[CCHDEVICENAME];
};

struct DISPLAYCONFIG_DESKTOP_IMAGE_INFO {
    POINTL PathSourceSize;
    RECTL DesktopImageRegion;
    RECTL DesktopImageClip;
};

struct DISPLAYCONFIG_RATIONAL {
    UINT32    Numerator;
    UINT32    Denominator;
};

struct DISPLAYCONFIG_2DREGION {
    UINT32 cx;
    UINT32 cy;
};

struct DISPLAYCONFIG_VIDEO_SIGNAL_INFO {
    UINT64 pixelRate;
    DISPLAYCONFIG_RATIONAL hSyncFreq;
    DISPLAYCONFIG_RATIONAL vSyncFreq;
    DISPLAYCONFIG_2DREGION activeSize;
    DISPLAYCONFIG_2DREGION totalSize;
    union {
        struct {
            UINT32 videoStandard :16;
            UINT32 vSyncFreqDivider :6;
            UINT32 reserved :10;
        } AdditionalSignalInfo;
        UINT32 videoStandard;
    } DUMMYUNIONNAME;
    DISPLAYCONFIG_SCANLINE_ORDERING scanLineOrdering;
};

struct DISPLAYCONFIG_TARGET_MODE {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO targetVideoSignalInfo;
};

struct DISPLAYCONFIG_SOURCE_MODE {
    UINT32 width;
    UINT32 height;
    DISPLAYCONFIG_PIXELFORMAT pixelFormat;
    POINTL position;
};

struct DISPLAYCONFIG_MODE_INFO {
    DISPLAYCONFIG_MODE_INFO_TYPE infoType;
    UINT32 id;
    LUID adapterId;
    union {
        DISPLAYCONFIG_TARGET_MODE targetMode;
        DISPLAYCONFIG_SOURCE_MODE sourceMode;
        DISPLAYCONFIG_DESKTOP_IMAGE_INFO desktopImageInfo;
    } DUMMYUNIONNAME;
};

struct DISPLAYCONFIG_PATH_SOURCE_INFO {
    LUID adapterId;
    UINT32 id;
    union {
        UINT32 modeInfoIdx;
        struct {
            UINT32 cloneGroupId :16;
            UINT32 sourceModeInfoIdx :16;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    UINT32 statusFlags;
};

struct DISPLAYCONFIG_PATH_TARGET_INFO {
    LUID adapterId;
    UINT32 id;
    union {
        UINT32 modeInfoIdx;
        struct {
            UINT32 desktopModeInfoIdx :16;
            UINT32 targetModeInfoIdx :16;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology;
    DISPLAYCONFIG_ROTATION rotation;
    DISPLAYCONFIG_SCALING scaling;
    DISPLAYCONFIG_RATIONAL refreshRate;
    DISPLAYCONFIG_SCANLINE_ORDERING scanLineOrdering;
    BOOL targetAvailable;
    UINT32 statusFlags;
};

struct DISPLAYCONFIG_PATH_INFO {
    DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo;
    DISPLAYCONFIG_PATH_TARGET_INFO targetInfo;
    UINT32 flags;
};

/* The following definitions and structures are taken
 * from here https://github.com/notr1ch/DWMCapture/blob/master/DWMCaptureSource.cpp */

enum D3DKMT_ESCAPETYPE {
    D3DKMT_ESCAPE_DRIVERPRIVATE = 0
};

struct D3DDDI_ESCAPEFLAGS {
    union {
        struct {
            UINT    Reserved : 31;
        };
        UINT        Value;
    };
};

struct D3DKMT_ESCAPE {
    D3D_HANDLE hAdapter;
    D3D_HANDLE hDevice;
    D3DKMT_ESCAPETYPE Type;
    D3DDDI_ESCAPEFLAGS Flags;
    VOID* pPrivateDriverData;
    UINT PrivateDriverDataSize;
    D3D_HANDLE hContext;
};

struct D3DKMT_OPENADAPTERFROMHDC {
    HDC hDc;
    D3D_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};

struct D3DKMT_CLOSEADAPTER {
    D3D_HANDLE hAdapter;
};

struct D3DKMT_OPENADAPTERFROMDEVICENAME {
    const WCHAR *pDeviceName;
    D3D_HANDLE hAdapter;
    LUID AdapterLuid;
};

struct D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME {
    WCHAR DeviceName[32];
    D3D_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};

struct QXLMonitorEscape {
    QXLMonitorEscape(DEVMODE* dev_mode)
    {
        ZeroMemory(&_head, sizeof(_head));
        _head.x = dev_mode->dmPosition.x;
        _head.y = dev_mode->dmPosition.y;
        _head.width = dev_mode->dmPelsWidth;
        _head.height = dev_mode->dmPelsHeight;
    }
    QXLHead _head;
};

struct QxlCustomEscapeObj : public QXLEscapeSetCustomDisplay {
    QxlCustomEscapeObj(uint32_t bitsPerPel, uint32_t width, uint32_t height)
    {
        xres = width;
        yres = height;
        bpp = bitsPerPel;
    }
};

struct WDDMCustomDisplayEscape {
    WDDMCustomDisplayEscape(DEVMODE* dev_mode)
    {
        _ioctl = QXL_ESCAPE_SET_CUSTOM_DISPLAY;
        _custom.bpp = dev_mode->dmBitsPerPel;
        _custom.xres = dev_mode->dmPelsWidth;
        _custom.yres = dev_mode->dmPelsHeight;
    }
    uint32_t                    _ioctl;
    QXLEscapeSetCustomDisplay   _custom;
};

struct WDDMMonitorConfigEscape {
    WDDMMonitorConfigEscape(DisplayMode* mode)
    {
        _ioctl = QXL_ESCAPE_MONITOR_CONFIG;
        _head.id = _head.surface_id = 0;
        _head.x = mode->get_pos_x();
        _head.y = mode->get_pos_y();
        _head.width = mode->get_width();
        _head.height = mode->get_height();
    }
    uint32_t    _ioctl;
    QXLHead     _head;
};

DisplayConfig* DisplayConfig::create_config()
{
    DisplayConfig* new_interface;
    /* Try to open a WDDM adapter.
    If that failed, assume we have an XPDM driver */
    try {
        new_interface = new  WDDMInterface();
    }
    catch (std::exception& e) {
        new_interface = new XPDMInterface();
    }
    return new_interface;
}

DisplayConfig::DisplayConfig()
    : _send_monitors_config(false)
{}

bool XPDMInterface::is_attached(DISPLAY_DEVICE* dev_info)
{
    return !!(dev_info->StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
}

bool XPDMInterface::set_monitor_state(LPCTSTR device_name, DEVMODE* dev_mode, MONITOR_STATE state)
{
    if (state == MONITOR_DETACHED) {
        dev_mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
        dev_mode->dmPelsWidth = 0;
        dev_mode->dmPelsHeight = 0;

        LONG status = ChangeDisplaySettingsEx(device_name, dev_mode, NULL, CDS_UPDATEREGISTRY, NULL);
        return (status == DISP_CHANGE_SUCCESSFUL);
    }
    return true;
}

LONG XPDMInterface::update_display_settings()
{
    return ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL);
}

bool XPDMInterface::update_dev_mode_position(LPCTSTR device_name,
                                             DEVMODE* dev_mode, LONG x, LONG y)
{
    dev_mode->dmPosition.x = x;
    dev_mode->dmPosition.y = y;
    dev_mode->dmFields |= DM_POSITION;
    vd_printf("setting %S at (%lu, %lu)", device_name, dev_mode->dmPosition.x,
        dev_mode->dmPosition.y);

    LONG status = ChangeDisplaySettingsEx(device_name, dev_mode, NULL,
                                          CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    return (status == DISP_CHANGE_SUCCESSFUL);
}

bool XPDMInterface::custom_display_escape(LPCTSTR device_name, DEVMODE* dev_mode)
{
    LONG        ret;
    NTSTATUS    Status (ERROR_SUCCESS);
    HDC         hdc = CreateDC(device_name, NULL, NULL, NULL);

    if (!hdc) {
        ret = ChangeDisplaySettingsEx(device_name, dev_mode, NULL, CDS_UPDATEREGISTRY, NULL);
        if (ret == DISP_CHANGE_BADMODE) {
            // custom resolution might not be set yet, use known resolution
            // FIXME: this causes client temporary resize... a
            // solution would involve passing custom resolution before
            // driver initialization, perhaps through registry
            dev_mode->dmPelsWidth = 640;
            dev_mode->dmPelsHeight = 480;
            ret = ChangeDisplaySettingsEx(device_name, dev_mode, NULL, CDS_UPDATEREGISTRY, NULL);
        }

        vd_printf("attach %ld", ret);
        if (!(hdc = CreateDC(device_name, NULL, NULL, NULL))) {
            vd_printf("failed to create DC");
            return false;
        }
    }

    QxlCustomEscapeObj custom_escape(dev_mode->dmBitsPerPel,
                                            dev_mode->dmPelsWidth, dev_mode->dmPelsHeight);

    int err = ExtEscape(hdc, QXL_ESCAPE_SET_CUSTOM_DISPLAY,
              sizeof(QXLEscapeSetCustomDisplay), (LPCSTR) &custom_escape, 0, NULL);
    if (err <= 0) {
        vd_printf("Can't set custom display, perhaps running with an older driver?");
    }

    if (!find_best_mode(device_name, dev_mode)) {
        Status = E_FAIL;
    }

    DeleteDC(hdc);
    return NT_SUCCESS(Status);
}

bool XPDMInterface::update_monitor_config(LPCTSTR device_name, DisplayMode* mode,
                                           DEVMODE* dev_mode)
{
    if (!mode || !mode->get_attached()) {
        return false;
    }

    QXLMonitorEscape monitor_config(dev_mode);
    HDC hdc(CreateDC(device_name, NULL, NULL, NULL));
    int err(0);

    if (!hdc || !_send_monitors_config) {
        return false;
    }

    err = ExtEscape(hdc, QXL_ESCAPE_MONITOR_CONFIG, sizeof(QXLHead),
                    (LPCSTR) &monitor_config, 0, NULL);
    if (err < 0) {
        vd_printf("%S can't update monitor config, may have old, old driver",
                  device_name);
    }
    DeleteDC(hdc);
    return (err >= 0);
}

bool XPDMInterface::find_best_mode(LPCTSTR Device, DEVMODE* dev_mode)
{
    DWORD closest_diff = -1;
    DWORD best = -1;

    // force refresh mode table
    DEVMODE test_dev_mode;
    ZeroMemory(&test_dev_mode, sizeof(test_dev_mode));
    test_dev_mode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(Device, 0xffffff, &test_dev_mode);

    //Find the closest size which will fit within the monitor
    for (DWORD i = 0; EnumDisplaySettings(Device, i, &test_dev_mode); i++) {
        if (dev_mode->dmPelsWidth > test_dev_mode.dmPelsWidth ||
            dev_mode->dmPelsHeight > test_dev_mode.dmPelsHeight ||
            dev_mode->dmBitsPerPel != test_dev_mode.dmBitsPerPel) {
            continue;
        }
        DWORD wdiff = dev_mode->dmPelsWidth - test_dev_mode.dmPelsWidth;
        DWORD hdiff = dev_mode->dmPelsHeight - test_dev_mode.dmPelsHeight;
        DWORD diff = wdiff * wdiff + hdiff * hdiff;
        if (diff < closest_diff) {
            closest_diff = diff;
            best = i;
        }
    }
    vd_printf("closest_diff at %lu best %lu", closest_diff, best);
    if (best == (DWORD) -1 || !EnumDisplaySettings(Device, best, dev_mode)) {
        return false;
    }

    //Change to the best fit
    LONG status = ChangeDisplaySettingsEx(Device, dev_mode, NULL,
                                          CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    return NT_SUCCESS(status);
}

WDDMInterface::WDDMInterface()
    : _pfnOpen_adapter_hdc(NULL)
    , _pfnClose_adapter(NULL)
    , _pfnEscape(NULL)
    , _pfnOpen_adapter_device_name(NULL)
    , _pfnOpen_adapter_gdi_name(NULL)
{
    LONG error(0);
    //Can we find the D3D calls we need?
    if (!init_d3d_api()) {
        throw std::exception();
    }

    //Initialize  CCD path stuff
    if (!_ccd.query_display_config()) {
        throw std::exception();
    }

    if (!_ccd.set_display_config(error)) {
        throw std::exception();
    }
}

bool WDDMInterface::is_attached(DISPLAY_DEVICE* dev_info)
{
    return _ccd.is_attached(dev_info->DeviceName);
}

bool WDDMInterface::set_monitor_state(LPCTSTR device_name, DEVMODE* dev_mode, MONITOR_STATE state)
{
   return  _ccd.set_path_state(device_name, state);
}

bool WDDMInterface::custom_display_escape(LPCTSTR device_name, DEVMODE* dev_mode)
{
    DISPLAYCONFIG_MODE_INFO* mode = _ccd.get_active_mode(device_name, false);
    if (!mode) {
        return false;
    }

    //Don't bother if we are already set to the new resolution
    if (mode->sourceMode.width == dev_mode->dmPelsWidth &&
        mode->sourceMode.height == dev_mode->dmPelsHeight) {
        return true;
    }

    vd_printf("updating %S resolution", device_name);

    WDDMCustomDisplayEscape wddm_escape(dev_mode);
    if (escape(device_name, &wddm_escape, sizeof(wddm_escape))) {
        return _ccd.update_mode_size(device_name, dev_mode);
    }

    vd_printf("(%dx%d)", mode->sourceMode.width, mode->sourceMode.height);
    return false;
}

bool WDDMInterface::update_monitor_config(LPCTSTR device_name, DisplayMode* display_mode,
                                           DEVMODE* dev_mode)
{
    if (!display_mode || !display_mode->get_attached()) {
        return false;
    }
    DISPLAYCONFIG_MODE_INFO* mode = _ccd.get_active_mode(device_name, false);
    if (!mode || !_send_monitors_config)
        return false;

    WDDMMonitorConfigEscape wddm_escape(display_mode);
    if (escape(device_name, &wddm_escape, sizeof(wddm_escape))) {
        //Update the path position
        return _ccd.update_mode_position(device_name, dev_mode);
    }

    vd_printf("%S failed", device_name);
    return false;

}

LONG WDDMInterface::update_display_settings()
{
    LONG error(0);
    //If we removed the primary monitor since the last call, we need to
    //reorder the other monitors, making the leftmost one the primary
    _ccd.verify_primary_position();
    _ccd.set_display_config(error);
    return error;
}

void WDDMInterface::update_config_path()
{
    _ccd.query_display_config();
}

bool WDDMInterface::update_dev_mode_position(LPCTSTR device_name, DEVMODE* dev_mode,
                                             LONG x, LONG y)
{
    dev_mode->dmPosition.x = x;
    dev_mode->dmPosition.y = y;
    return _ccd.update_mode_position(device_name, dev_mode);
}

bool WDDMInterface::init_d3d_api()
{
    HMODULE hModule = GetModuleHandle(L"gdi32.dll");

    //Look for the gdi32 functions we need to perform driver escapes
    if (!hModule) {
        vd_printf("something wildly wrong as we can't open gdi32.dll");
        return false;
    }

    do {
        _pfnClose_adapter = (PFND3DKMT_CLOSEADAPTER)
            GetProcAddress(hModule, "D3DKMTCloseAdapter");
        if (!_pfnClose_adapter) {
            break;
        }

        _pfnEscape = (PFND3DKMT_ESCAPE) GetProcAddress(hModule, "D3DKMTEscape");
        if (!_pfnEscape) {
            break;
        }

        _pfnOpen_adapter_hdc = (PFND3DKMT_OPENADAPTERFROMHDC)
            GetProcAddress(hModule, "D3DKMTOpenAdapterFromHdc");
        if (!_pfnOpen_adapter_hdc) {
            break;
        }

        _pfnOpen_adapter_device_name = (PFND3DKMT_OPENADAPTERFROMDEVICENAME)
            GetProcAddress(hModule, "D3DKMTOpenAdapterFromDeviceName");
        if (!_pfnOpen_adapter_device_name) {
            break;
        }

        _pfnOpen_adapter_gdi_name = (PFND3DKMT_OPENADAPTERFROMGDIDISPLAYNAME)
            GetProcAddress(hModule, "D3DKMTOpenAdapterFromGdiDisplayName");
        if (!_pfnOpen_adapter_gdi_name) {
            break;
        }

    }
    while(0);

    //Did we get them ?
    if (!_pfnClose_adapter || !_pfnOpen_adapter_hdc || !_pfnEscape)  {
        return false;
    }
    return true;
}

D3D_HANDLE WDDMInterface::adapter_handle(LPCTSTR device_name)
{
    D3D_HANDLE hAdapter(0);

    //For some reason, this call will occasionally fail.
    if ((hAdapter = handle_from_DC(device_name))) {
        return hAdapter;
    }
        //So try other available methods.
    if (_pfnOpen_adapter_device_name && (hAdapter = handle_from_device_name(device_name))) {
        return hAdapter;
    }
    //One last chance to open this guy
    if (_pfnOpen_adapter_gdi_name) {
        hAdapter = handle_from_GDI_name(device_name);
    }

    if (!hAdapter) {
        vd_printf("failed to open adapter %S", device_name);
    }

    return hAdapter;
}

D3D_HANDLE WDDMInterface::handle_from_DC(LPCTSTR adapter_name)
{
    NTSTATUS status;
    D3DKMT_OPENADAPTERFROMHDC open_data;
    HDC hDc(CreateDC(adapter_name, NULL, NULL, NULL));

    if (!hDc) {
        vd_printf("%S CreateDC failed with %lu", adapter_name, GetLastError());
        return 0;
    }

    ZeroMemory(&open_data, sizeof(D3DKMT_OPENADAPTERFROMHDC));
    open_data.hDc = hDc;

    if (!NT_SUCCESS(status = _pfnOpen_adapter_hdc(&open_data))) {
        vd_printf("%S open adapter from hdc failed with %lu", adapter_name,
            status);
        open_data.hAdapter = 0;
    }

    DeleteDC(hDc);
    return open_data.hAdapter;
}

D3D_HANDLE WDDMInterface::handle_from_device_name(LPCTSTR adapter_name)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME display_name_data;
    NTSTATUS  status;

    ZeroMemory(&display_name_data, sizeof(display_name_data));
    display_name_data.pDeviceName = adapter_name;

    if (NT_SUCCESS(status = _pfnOpen_adapter_device_name(&display_name_data))) {
        return display_name_data.hAdapter;
    }

    vd_printf("%S failed with 0x%lx", adapter_name, status);
    return 0;
}

D3D_HANDLE WDDMInterface::handle_from_GDI_name(LPCTSTR adapter_name)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME gdi_display_name;
    NTSTATUS status;

    ZeroMemory(&gdi_display_name, sizeof(gdi_display_name));
    wcsncpy(gdi_display_name.DeviceName, adapter_name, SPICE_N_ELEMENTS(gdi_display_name.DeviceName));

    if (NT_SUCCESS(status = _pfnOpen_adapter_gdi_name(&gdi_display_name))) {
        return  gdi_display_name.hAdapter;
    }

    vd_printf("%S aurrrgghh nothing works..error  is 0x%lx", adapter_name,
            status);
    return 0;
}

void WDDMInterface::close_adapter(D3D_HANDLE handle)
{
    D3DKMT_CLOSEADAPTER closeData;
    if (handle) {
        closeData.hAdapter = handle;
        _pfnClose_adapter(&closeData);
    }
}

bool WDDMInterface::escape(LPCTSTR device_name, void* data, UINT size_data)
{
    D3DKMT_ESCAPE   escapeData;
    NTSTATUS        status;
    D3D_HANDLE   hAdapter(0);

    if (!(hAdapter = adapter_handle(device_name)))
        return false;

    escapeData.hAdapter = hAdapter;
    escapeData.hDevice = 0;
    escapeData.hContext = 0;
    escapeData.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escapeData.Flags.Value = 0;
    escapeData.pPrivateDriverData = data;
    escapeData.PrivateDriverDataSize = size_data;

    status = _pfnEscape(&escapeData);

    if (!NT_SUCCESS(status)) {
        vd_printf("this should never happen. Status is 0x%lx", status);
    }

    //Close the handle to this device
    close_adapter(hAdapter);
    return NT_SUCCESS(status);
}

CCD::CCD()
    :_numPathElements(0)
    ,_numModeElements(0)
    ,_pPathInfo(NULL)
    ,_pModeInfo(NULL)
    ,_pfnGetDeviceInfo(NULL)
    ,_pfnGetDisplayConfigBufferSizes(NULL)
    ,_pfnQueryDisplayConfig(NULL)
    ,_pfnSetDisplayConfig(NULL)
    ,_primary_detached(false)
    ,_path_state(PATH_UPDATED)
{
    if (load_api()) {
        get_config_buffers();
    }
    else {
        throw std::exception();
    }
}

CCD::~CCD()
{
    free_config_buffers();
}

bool CCD::query_display_config()
{
    LONG query_error(ERROR_SUCCESS);
    if (!get_config_buffers())
        return false;
    //Until we get it or error != ERROR_INSUFFICIENT_BUFFER
    do {
        query_error = _pfnQueryDisplayConfig(QDC_ALL_PATHS, &_numPathElements, _pPathInfo,
            &_numModeElements, _pModeInfo, NULL);

        // if ERROR_INSUFFICIENT_BUFFER need to retry QueryDisplayConfig
        // to get a new set of config buffers
        //(see https://msdn.microsoft.com/en-us/library/windows/hardware/ff569215(v=vs.85).aspx )
        if (query_error) {
             if (query_error == ERROR_INSUFFICIENT_BUFFER) {
                if (!get_config_buffers())
                    return false;
            } else {
                vd_printf("failed QueryDisplayConfig with 0x%lx", query_error);
                return false;
            }
        }
    } while(query_error);
    _path_state = PATH_CURRENT;
    return true;
}

DISPLAYCONFIG_MODE_INFO* CCD::get_active_mode(LPCTSTR device_name, bool return_on_error)
{
    DISPLAYCONFIG_PATH_INFO* active_path;

    active_path = get_device_path(device_name, true);

    if (!active_path ) {
        vd_printf("%S failed", device_name);
        return NULL;
    }
    return &_pModeInfo[active_path->sourceInfo.modeInfoIdx];
}

bool CCD::set_display_config(LONG & error) {

    debug_print_config("Before SetDisplayConfig");

    if (_path_state == PATH_CURRENT) {
        vd_printf("path states says nothing changed");
        return true;
    }

    if (!(error = _pfnSetDisplayConfig(_numPathElements, _pPathInfo,
            _numModeElements, _pModeInfo,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_FORCE_MODE_ENUMERATION | SDC_SAVE_TO_DATABASE))) {
        return true;
    }

    vd_printf("failed SetDisplayConfig with 0x%lx", error);
    debug_print_config("After failed SetDisplayConfig");
    return false;
}

DISPLAYCONFIG_PATH_INFO* CCD::get_device_path(LPCTSTR device_name, bool bActive)
{
    //Search for device's active path
    for (UINT32 i = 0; i < _numPathElements; i++) {
        DISPLAYCONFIG_PATH_INFO* path_info = &_pPathInfo[i];

        //if bActive, return only paths that are currently active
        if (bActive && !is_active_path(path_info))
            continue;
        if (!is_device_path(device_name, path_info))
            continue;
        return path_info;
    }
    return NULL;
}

void CCD::debug_print_config(const char* prefix)
{
    TCHAR dev_name[CCHDEVICENAME];
    for (UINT32 i = 0; i < _numPathElements; i++) {
        DISPLAYCONFIG_PATH_INFO* path_info = &_pPathInfo[i];
        if (!(path_info->flags & DISPLAYCONFIG_PATH_ACTIVE))
            continue;
        get_device_name_config(path_info, dev_name);

        if (path_info->sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            vd_printf("%S  [%s] This path is active but has invalid mode set.",
                dev_name, prefix);
            continue;
        }
        DISPLAYCONFIG_MODE_INFO* mode = &_pModeInfo[path_info->sourceInfo.modeInfoIdx];
        vd_printf("%S [%s] (%ld,%ld) (%ux%u).", dev_name, prefix,
            mode->sourceMode.position.x, mode->sourceMode.position.y,
            mode->sourceMode.width, mode->sourceMode.height);
    }
}

bool CCD::load_api()
{
    HMODULE hModule = GetModuleHandle(L"user32.dll");
    if(!hModule) {
        return false;
    }

    bool bFound_all(false);
    do {
        if (!(_pfnGetDeviceInfo = (PDISPLAYCONFIG_GETDEVICEINFO)
            GetProcAddress(hModule, "DisplayConfigGetDeviceInfo"))) {
            break;
        }

        if (!(_pfnGetDisplayConfigBufferSizes = (PGETDISPLAYCONFIG_BUFFERSIZES)
            GetProcAddress(hModule, "GetDisplayConfigBufferSizes"))) {
            break;
        }

        if (!(_pfnQueryDisplayConfig = (PQUERYDISPLAYCONFIG)
            GetProcAddress(hModule, "QueryDisplayConfig"))) {
            break;
        }

        if (!(_pfnSetDisplayConfig = (PSETDISPLAYCONFIG)
            GetProcAddress(hModule, "SetDisplayConfig"))) {
            break;
        }
        bFound_all = true;
    }
    while(0);

    return bFound_all;
}

bool CCD::get_config_buffers()
{
    //Get Config Buffer Sizes
    free_config_buffers();
    LONG error(ERROR_SUCCESS);
    error = _pfnGetDisplayConfigBufferSizes(QDC_ALL_PATHS, &_numPathElements,
                                            &_numModeElements);
    if (error == ERROR_NOT_SUPPORTED) {
        vd_printf("GetDisplayConfigBufferSizes failed, missing WDDM");
        throw std::exception();
    }
    if (error) {
        vd_printf("GetDisplayConfigBufferSizes failed with 0x%lx", error);
        return false;
    }

    //Allocate arrays
    _pPathInfo = new(std::nothrow) DISPLAYCONFIG_PATH_INFO[_numPathElements];
    _pModeInfo = new(std::nothrow) DISPLAYCONFIG_MODE_INFO[_numModeElements];

    if (!_pPathInfo || !_pModeInfo) {
        vd_printf("OOM ");
        free_config_buffers();
        return false;
    }

    ///clear the above arrays
    ZeroMemory(_pPathInfo, sizeof(DISPLAYCONFIG_PATH_INFO) * _numPathElements);
    ZeroMemory(_pModeInfo, sizeof(DISPLAYCONFIG_MODE_INFO) * _numModeElements);
    return true;
}

void CCD::free_config_buffers()
{
    delete[] _pModeInfo;
    _pModeInfo = NULL;
    delete[] _pPathInfo;
    _pPathInfo = NULL;
    _numModeElements = _numPathElements = 0;
}

bool CCD::get_device_name_config(DISPLAYCONFIG_PATH_INFO* path, LPTSTR dev_name)
{
    LONG error(ERROR_SUCCESS);

    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name;
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
    source_name.header.adapterId = path->sourceInfo.adapterId;
    source_name.header.id = path->sourceInfo.id;

    error = _pfnGetDeviceInfo(&source_name.header);
    if (error) {
        vd_printf("DisplayConfigGetDeviceInfo failed with %lu", error);
        return false;
    }
    memcpy((void *)dev_name, source_name.viewGdiDeviceName, CCHDEVICENAME * sizeof(TCHAR));
    return true;
}

bool CCD::is_device_path(LPCTSTR device_name, DISPLAYCONFIG_PATH_INFO* path)
{
    //Does this path belong to device_name?
    TCHAR dev_name[CCHDEVICENAME];
    if (!get_device_name_config(path, dev_name)) {
        return false;
    }
    if (_tcscmp(dev_name, device_name)) {
        return false;
    }
    return true;
}

// If we have detached the primary monitor, then we need to reset the positions of the remaining
// monitors to insure that at least one is positioned at (0,0)
// Windows specify that there must be such a monitor which is considered the primary one
void CCD::verify_primary_position()
{
    LONG leftmost_x(LONG_MAX);
    LONG leftmost_y(LONG_MAX);
    if (!_primary_detached) {
        return;
    }
    _primary_detached = false;

    for (UINT32 i = 0; i < _numPathElements; i++) {
        DISPLAYCONFIG_PATH_INFO* path_info = &_pPathInfo[i];
        if (!is_active_path(path_info))
            continue;

        const POINTL& position(_pModeInfo[path_info->sourceInfo.modeInfoIdx].sourceMode.position);
        // we already have a primary monitor so we have nothing to do
        if (position.x == 0 && position.y == 0)
            return;
        if (leftmost_x > position.x) {
            leftmost_x = position.x;
            leftmost_y = position.y;
        }
        // in case there are more monitors on the left most, choose the top one
        if (leftmost_x == position.x && leftmost_y > position.y)
            leftmost_y = position.y;
    }

    // update all active monitors adjusting the choosen monitor to (0,0)
    for (UINT32 i = 0; i < _numPathElements; i++) {
        DISPLAYCONFIG_PATH_INFO* path_info = &_pPathInfo[i];
        if (!is_active_path(path_info))
            continue;
        POINTL& position(_pModeInfo[path_info->sourceInfo.modeInfoIdx].sourceMode.position);
        vd_printf("setting mode x to %lu", position.x);
        position.x -= leftmost_x;
        position.y -= leftmost_y;
    }
    _path_state = PATH_UPDATED;
}

bool CCD::update_mode_position(LPCTSTR device_name, DEVMODE* dev_mode)
{
    DISPLAYCONFIG_MODE_INFO* mode = get_active_mode(device_name, false);
    if (!mode)
        return false;

    mode->sourceMode.position.x = dev_mode->dmPosition.x;
    mode->sourceMode.position.y = dev_mode->dmPosition.y;
    vd_printf("%S updated path mode to (%lu, %lu) - (%u x%u)",
        device_name,
        mode->sourceMode.position.x, mode->sourceMode.position.y,
        mode->sourceMode.width, mode->sourceMode.height);
    _path_state = PATH_UPDATED;
    return true;

}

bool CCD::update_mode_size(LPCTSTR device_name, DEVMODE* dev_mode)
{
    DISPLAYCONFIG_MODE_INFO* mode = get_active_mode(device_name, false);
    if (!mode) {
        return false;
    }

    mode->sourceMode.width = dev_mode->dmPelsWidth;
    mode->sourceMode.height = dev_mode->dmPelsHeight;
    vd_printf("%S updated path mode to (%lu, %lu - (%u x %u)",
        device_name,
        mode->sourceMode.position.x, mode->sourceMode.position.y,
        mode->sourceMode.width, mode->sourceMode.height);
    _path_state = PATH_UPDATED;
    return true;
}

void CCD::update_detached_primary_state(LPCTSTR device_name, DISPLAYCONFIG_PATH_INFO * path_info)
{
    DISPLAYCONFIG_MODE_INFO* mode(get_active_mode(device_name, false));

    //will need to reset monitor positions if primary detached
    path_info->flags = path_info->flags & ~DISPLAYCONFIG_PATH_ACTIVE;
    if (!mode|| mode->sourceMode.position.x != 0 || mode->sourceMode.position.y != 0) {
        return ;
    }
    _primary_detached = true;
}

bool CCD::set_path_state(LPCTSTR device_name, MONITOR_STATE new_state)
{
    DISPLAYCONFIG_PATH_INFO* path(get_device_path(device_name, false));
    MONITOR_STATE current_path_state(MONITOR_DETACHED);
    LONG error(0);

    if (is_active_path(path)) {
        current_path_state = MONITOR_ATTACHED;
    }

    //If state didn't change, nothing to do
    if (current_path_state == new_state ) {
        return true;
    }

    if (!path) {
        return false;
    }

    _path_state = PATH_UPDATED;
    if (new_state == MONITOR_DETACHED) {
        update_detached_primary_state(device_name, path);
    } else {
        path->flags = path->flags | DISPLAYCONFIG_PATH_ACTIVE;
        set_display_config(error);
    }
    return true;
}

bool CCD::is_attached(LPCTSTR device_name)
{
    return is_active_path(get_device_path(device_name, false));
}

bool CCD::is_active_path(DISPLAYCONFIG_PATH_INFO * path)
{
    return (path && (path->flags & DISPLAYCONFIG_PATH_ACTIVE) &&
        (path->sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID));
}
