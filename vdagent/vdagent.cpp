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

#include "vdcommon.h"
#include "virtio_vdi_port.h"
#include "pci_vdi_port.h"
#include "desktop_layout.h"
#include "display_setting.h"
#include "ximage.h"
#undef max
#undef min
#include <wtsapi32.h>
#include <lmcons.h>
#include <queue>
#include <set>

#define VD_AGENT_LOG_PATH       TEXT("%svdagent.log")
#define VD_AGENT_WINCLASS_NAME  TEXT("VDAGENT")
#define VD_INPUT_INTERVAL_MS    20
#define VD_TIMER_ID             1
#define VD_CLIPBOARD_TIMEOUT_MS 10000
#define VD_CLIPBOARD_FORMAT_MAX_TYPES 16

//FIXME: extract format/type stuff to win_vdagent_common for use by windows\platform.cpp as well
typedef struct VDClipboardFormat {
    uint32_t format;
    uint32_t types[VD_CLIPBOARD_FORMAT_MAX_TYPES];
} VDClipboardFormat;

VDClipboardFormat clipboard_formats[] = {
    {CF_UNICODETEXT, {VD_AGENT_CLIPBOARD_UTF8_TEXT, 0}},
    //FIXME: support more image types
    {CF_DIB, {VD_AGENT_CLIPBOARD_IMAGE_PNG, VD_AGENT_CLIPBOARD_IMAGE_BMP, 0}},
};

#define clipboard_formats_count (sizeof(clipboard_formats) / sizeof(clipboard_formats[0]))

typedef struct ImageType {
    uint32_t type;
    DWORD cximage_format;
} ImageType;

static ImageType image_types[] = {
    {VD_AGENT_CLIPBOARD_IMAGE_PNG, CXIMAGE_FORMAT_PNG},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, CXIMAGE_FORMAT_BMP},
};

typedef struct ALIGN_VC VDIChunk {
    VDIChunkHeader hdr;
    uint8_t data[0];
} ALIGN_GCC VDIChunk;

#define VD_MESSAGE_HEADER_SIZE (sizeof(VDIChunk) + sizeof(VDAgentMessage))

enum {
    VD_EVENT_CONTROL = 0,
    VD_STATIC_EVENTS_COUNT // Must be last
};

class VDAgent {
public:
    static VDAgent* get();
    ~VDAgent();
    bool run();

private:
    VDAgent();
    void input_desktop_message_loop();
    bool handle_mouse_event(VDAgentMouseState* state);
    bool handle_announce_capabilities(VDAgentAnnounceCapabilities* announce_capabilities,
                                      uint32_t msg_size);
    bool handle_mon_config(VDAgentMonitorsConfig* mon_config, uint32_t port);
    bool handle_clipboard(VDAgentClipboard* clipboard, uint32_t size);
    bool handle_clipboard_grab(VDAgentClipboardGrab* clipboard_grab, uint32_t size);
    bool handle_clipboard_request(VDAgentClipboardRequest* clipboard_request);
    void handle_clipboard_release();
    bool handle_display_config(VDAgentDisplayConfig* display_config, uint32_t port);
    void handle_port_in();
    void handle_port_out();
    void handle_chunk(VDIChunk* chunk);
    void on_clipboard_grab();
    void on_clipboard_request(UINT format);
    void on_clipboard_release();
    DWORD get_buttons_change(DWORD last_buttons_state, DWORD new_buttons_state,
                             DWORD mask, DWORD down_flag, DWORD up_flag);
    static HGLOBAL utf8_alloc(LPCSTR data, int size);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static DWORD WINAPI event_thread_proc(LPVOID param);
    static void dispatch_message(VDAgentMessage* msg, uint32_t port);
    uint32_t get_clipboard_format(uint32_t type);
    uint32_t get_clipboard_type(uint32_t format);
    DWORD get_cximage_format(uint32_t type);
    enum { owner_none, owner_guest, owner_client };
    void set_clipboard_owner(int new_owner);
    enum { CONTROL_STOP, CONTROL_DESKTOP_SWITCH, CONTROL_LOGON };
    void set_control_event(int control_command);
    void handle_control_event();
    VDIChunk* new_chunk(DWORD bytes = 0);
    void enqueue_chunk(VDIChunk* msg);
    bool write_message(uint32_t type, uint32_t size, void* data);
    bool write_clipboard(VDAgentMessage* msg, uint32_t size);
    bool init_vdi_port();
    bool send_input();
    void set_display_depth(uint32_t depth);
    void load_display_setting();
    bool send_announce_capabilities(bool request);
    void cleanup();

private:
    static VDAgent* _singleton;
    HWND _hwnd;
    HWND _hwnd_next_viewer;
    int _clipboard_owner;
    DWORD _buttons_state;
    ULONG _mouse_x;
    ULONG _mouse_y;
    INPUT _input;
    DWORD _input_time;
    HANDLE _control_event;
    HANDLE _clipboard_event;
    HANDLE* _events;
    VDAgentMessage* _in_msg;
    uint32_t _in_msg_pos;
    uint32_t _events_count;
    bool _pending_input;
    bool _running;
    bool _desktop_switch;
    DesktopLayout* _desktop_layout;
    DisplaySetting _display_setting;
    VDIPort* _vdi_port;
    mutex_t _control_mutex;
    mutex_t _message_mutex;
    std::queue<int> _control_queue;
    std::queue<VDIChunk*> _message_queue;

    bool _logon_desktop;
    bool _display_setting_initialized;
    bool _logon_occured;

    uint32_t *_client_caps;
    uint32_t _client_caps_size;

    std::set<uint32_t> _grab_types;

    VDLog* _log;
};

VDAgent* VDAgent::_singleton = NULL;

VDAgent* VDAgent::get()
{
    if (!_singleton) {
        _singleton = new VDAgent();
    }
    return _singleton;
}

VDAgent::VDAgent()
    : _hwnd (NULL)
    , _hwnd_next_viewer (NULL)
    , _clipboard_owner (owner_none)
    , _buttons_state (0)
    , _mouse_x (0)
    , _mouse_y (0)
    , _input_time (0)
    , _control_event (NULL)
    , _clipboard_event (NULL)
    , _events (NULL)
    , _in_msg (NULL)
    , _in_msg_pos (0)
    , _events_count (0)
    , _pending_input (false)
    , _running (false)
    , _desktop_switch (false)
    , _desktop_layout (NULL)
    , _display_setting (VD_AGENT_REGISTRY_KEY)
    , _vdi_port (NULL)
    , _logon_desktop (false)
    , _display_setting_initialized (false)
    , _client_caps (NULL)
    , _client_caps_size (0)
    , _log (NULL)
{
    TCHAR log_path[MAX_PATH];
    TCHAR temp_path[MAX_PATH];

    if (GetTempPath(MAX_PATH, temp_path)) {
        swprintf_s(log_path, MAX_PATH, VD_AGENT_LOG_PATH, temp_path);
        _log = VDLog::get(log_path);
    }
    ZeroMemory(&_input, sizeof(INPUT));
    MUTEX_INIT(_control_mutex);
    MUTEX_INIT(_message_mutex);

    _singleton = this;
}

VDAgent::~VDAgent()
{
    delete _events;
    delete _log;
    delete[] _client_caps;
}

DWORD WINAPI VDAgent::event_thread_proc(LPVOID param)
{
    HANDLE desktop_event = OpenEvent(SYNCHRONIZE, FALSE, L"WinSta0_DesktopSwitch");
    if (!desktop_event) {
        vd_printf("OpenEvent() failed: %lu", GetLastError());
        return 1;
    }
    while (_singleton->_running) {
        DWORD wait_ret = WaitForSingleObject(desktop_event, INFINITE);
        switch (wait_ret) {
        case WAIT_OBJECT_0:
            _singleton->set_control_event(CONTROL_DESKTOP_SWITCH);
            break;
        case WAIT_TIMEOUT:
        default:
            vd_printf("WaitForSingleObject(): %lu", wait_ret);
        }
    }
    CloseHandle(desktop_event);
    return 0;
}

bool VDAgent::run()
{
    DWORD session_id;
    DWORD event_thread_id;
    HANDLE event_thread;
    WNDCLASS wcls;

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        vd_printf("ProcessIdToSessionId failed %lu", GetLastError());
        return false;
    }
    vd_printf("***Agent started in session %lu***", session_id);
    log_version();
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        vd_printf("SetPriorityClass failed %lu", GetLastError());
    }
    if (!SetProcessShutdownParameters(0x100, 0)) {
        vd_printf("SetProcessShutdownParameters failed %lu", GetLastError());
    }
    _control_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    _clipboard_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!_control_event || !_clipboard_event) {
        vd_printf("CreateEvent() failed: %lu", GetLastError());
        cleanup();
        return false;
    }
    memset(&wcls, 0, sizeof(wcls));
    wcls.lpfnWndProc = &VDAgent::wnd_proc;
    wcls.lpszClassName = VD_AGENT_WINCLASS_NAME;
    if (!RegisterClass(&wcls)) {
        vd_printf("RegisterClass() failed: %lu", GetLastError());
        cleanup();
        return false;
    }
    _desktop_layout = new DesktopLayout();
    if (_desktop_layout->get_display_count() == 0) {
        vd_printf("No QXL devices!");
    }
    if (!init_vdi_port()) {
        vd_printf("Failed to create VDIPort instance");
        cleanup();
        return false;
    }
    _running = true;
    event_thread = CreateThread(NULL, 0, event_thread_proc, NULL, 0, &event_thread_id);
    if (!event_thread) {
        vd_printf("CreateThread() failed: %lu", GetLastError());
        cleanup();
        return false;
    }
    send_announce_capabilities(true);

    _events_count = VD_STATIC_EVENTS_COUNT + _vdi_port->get_num_events();
    _events = new HANDLE[_events_count];
    ZeroMemory(_events, _events_count);
    _events[VD_EVENT_CONTROL] = _control_event;
    _vdi_port->fill_events(&_events[VD_STATIC_EVENTS_COUNT]);
    vd_printf("Connected to server");

    while (_running) {
        input_desktop_message_loop();
        if (_clipboard_owner == owner_guest) {
            set_clipboard_owner(owner_none);
        }
    }
    vd_printf("Agent stopped");
    CloseHandle(event_thread);
    cleanup();
    return true;
}

void VDAgent::cleanup()
{
    CloseHandle(_control_event);
    CloseHandle(_clipboard_event);
    delete _vdi_port;
    delete _desktop_layout;
}

void VDAgent::set_control_event(int control_command)
{
    MUTEX_LOCK(_control_mutex);
    _control_queue.push(control_command);
    if (_control_event && !SetEvent(_control_event)) {
        vd_printf("SetEvent() failed: %lu", GetLastError());
    }
    MUTEX_UNLOCK(_control_mutex);
}

void VDAgent::handle_control_event()
{
    MUTEX_LOCK(_control_mutex);
    while (_control_queue.size()) {
        int control_command = _control_queue.front();
        _control_queue.pop();
        vd_printf("Control command %d", control_command);
        switch (control_command) {
        case CONTROL_STOP:
            _running = false;
            break;
        case CONTROL_DESKTOP_SWITCH:
            _desktop_switch = true;
            break;
        case CONTROL_LOGON:
            vd_printf("session logon");
            // loading the display settings for the current session's logged on user only
            // after 1) we receive logon event, and 2) the desktop switched from Winlogon
            if (!_logon_desktop) {
                vd_printf("LOGON display setting");
                _display_setting.load();
            } else {
                _logon_occured = true;
            }
            break;
        default:
            vd_printf("Unsupported control command %u", control_command);
        }
    }
    MUTEX_UNLOCK(_control_mutex);
}

void VDAgent::input_desktop_message_loop()
{
    TCHAR desktop_name[MAX_PATH];
    DWORD wait_ret;
    HDESK hdesk;
    MSG msg;

    hdesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hdesk) {
        vd_printf("OpenInputDesktop() failed: %lu", GetLastError());
        _running = false;
        return;
    }
    if (!SetThreadDesktop(hdesk)) {
        vd_printf("SetThreadDesktop failed %lu", GetLastError());
        _running = false;
        return;
    }
    if (GetUserObjectInformation(hdesk, UOI_NAME, desktop_name, sizeof(desktop_name), NULL)) {
        vd_printf("Desktop: %S", desktop_name);
    } else {
        vd_printf("GetUserObjectInformation failed %lu", GetLastError());
    }

    // loading the display settings for the current session's logged on user only
    // after 1) we receive logon event, and 2) the desktop switched from Winlogon
    if (_tcscmp(desktop_name, TEXT("Winlogon")) == 0) {
        _logon_desktop = true;
    } else {
        // first load after connection
        if (!_display_setting_initialized) {
            vd_printf("First display setting");
            _display_setting.load();
            _display_setting_initialized = true;
        } else if (_logon_occured && _logon_desktop) {
            vd_printf("LOGON display setting");
            _display_setting.load();
        }
        _logon_occured = false;
        _logon_desktop = false;
    }

    _hwnd = CreateWindow(VD_AGENT_WINCLASS_NAME, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!_hwnd) {
        vd_printf("CreateWindow() failed: %lu", GetLastError());
        _running = false;
        return;
    }
    if (!WTSRegisterSessionNotification(_hwnd, NOTIFY_FOR_ALL_SESSIONS)) {
        vd_printf("WTSRegisterSessionNotification() failed: %lu", GetLastError());
    }
    _hwnd_next_viewer = SetClipboardViewer(_hwnd);
    while (_running && !_desktop_switch) {
        int cont_read = _vdi_port->read();
        int cont_write = _vdi_port->write();
        bool cont = false;

        if (cont_read >= 0 && cont_write >= 0) {
            cont = cont_read || cont_write;
        } else if (cont_read == VDI_PORT_ERROR || cont_write == VDI_PORT_ERROR) {
            vd_printf("VDI Port error, read %d write %d", cont_read, cont_write);
            _running = false;
            break;
        } else if (cont_read == VDI_PORT_RESET || cont_write == VDI_PORT_RESET) {
            vd_printf("VDI Port reset, read %d write %d", cont_read, cont_write);
            _running = false;
            break;
        }
        if (cont_read) {
            handle_port_in();
        }
        if (cont_write) {
            handle_port_out();
        }

        wait_ret = MsgWaitForMultipleObjectsEx(_events_count, _events, cont ? 0 : INFINITE,
                                               QS_ALLINPUT, MWMO_ALERTABLE);
        if (wait_ret == WAIT_OBJECT_0 + _events_count) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            continue;
        }
        switch (wait_ret) {
        case WAIT_OBJECT_0 + VD_EVENT_CONTROL:
            handle_control_event();
            break;
        case WAIT_IO_COMPLETION:
        case WAIT_TIMEOUT:
            break;
        default:
            DWORD vdi_event = wait_ret - VD_STATIC_EVENTS_COUNT - WAIT_OBJECT_0;
            if (vdi_event >= 0 && vdi_event < _vdi_port->get_num_events()) {
                _running = _vdi_port->handle_event(vdi_event);
            } else {
                vd_printf("MsgWaitForMultipleObjectsEx failed: %lu %lu", wait_ret, GetLastError());
                _running = false;
            }
        }
    }
    _desktop_switch = false;
    if (_pending_input) {
        KillTimer(_hwnd, VD_TIMER_ID);
        _pending_input = false;
    }
    ChangeClipboardChain(_hwnd, _hwnd_next_viewer);
    WTSUnRegisterSessionNotification(_hwnd);
    DestroyWindow(_hwnd);
    CloseDesktop(hdesk);
}

DWORD VDAgent::get_buttons_change(DWORD last_buttons_state, DWORD new_buttons_state,
                                  DWORD mask, DWORD down_flag, DWORD up_flag)
{
    DWORD ret = 0;
    if (!(last_buttons_state & mask) && (new_buttons_state & mask)) {
        ret = down_flag;
    } else if ((last_buttons_state & mask) && !(new_buttons_state & mask)) {
        ret = up_flag;
    }
    return ret;
}

bool VDAgent::send_input()
{
    bool ret = true;
    _desktop_layout->lock();
    if (_pending_input) {
        if (KillTimer(_hwnd, VD_TIMER_ID)) {
            _pending_input = false;
        } else {
            vd_printf("KillTimer failed: %lu", GetLastError());
            _running = false;
            _desktop_layout->unlock();
            return false;
        }
    }
    if (!SendInput(1, &_input, sizeof(INPUT))) {
        DWORD err = GetLastError();
        // Don't stop agent due to UIPI blocking, which is usually only for specific windows
        // of system security applications (anti-viruses etc.)
        if (err != ERROR_SUCCESS && err != ERROR_ACCESS_DENIED) {
            vd_printf("SendInput failed: %lu", err);
            ret = _running = false;
        }
    }
    _input_time = GetTickCount();
    _desktop_layout->unlock();
    return ret;
}

bool VDAgent::handle_mouse_event(VDAgentMouseState* state)
{
    DisplayMode* mode = NULL;
    DWORD mouse_move = 0;
    DWORD buttons_change = 0;
    DWORD mouse_wheel = 0;
    bool ret = true;

    ASSERT(_desktop_layout);
    _desktop_layout->lock();
    if (state->display_id < _desktop_layout->get_display_count()) {
        mode = _desktop_layout->get_display(state->display_id);
    }
    if (!mode || !mode->get_attached()) {
        _desktop_layout->unlock();
        return true;
    }
    ZeroMemory(&_input, sizeof(INPUT));
    _input.type = INPUT_MOUSE;
    if (state->x != _mouse_x || state->y != _mouse_y) {
        _mouse_x = state->x;
        _mouse_y = state->y;
        mouse_move = MOUSEEVENTF_MOVE;
        _input.mi.dx = (mode->get_pos_x() + _mouse_x) * 0xffff /
                       _desktop_layout->get_total_width();
        _input.mi.dy = (mode->get_pos_y() + _mouse_y) * 0xffff /
                       _desktop_layout->get_total_height();
    }
    if (state->buttons != _buttons_state) {
        buttons_change = get_buttons_change(_buttons_state, state->buttons, VD_AGENT_LBUTTON_MASK,
                                            MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP) |
                         get_buttons_change(_buttons_state, state->buttons, VD_AGENT_MBUTTON_MASK,
                                            MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP) |
                         get_buttons_change(_buttons_state, state->buttons, VD_AGENT_RBUTTON_MASK,
                                            MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
        mouse_wheel = get_buttons_change(_buttons_state, state->buttons,
                                         VD_AGENT_UBUTTON_MASK | VD_AGENT_DBUTTON_MASK,
                                         MOUSEEVENTF_WHEEL, 0);
        if (mouse_wheel) {
            if (state->buttons & VD_AGENT_UBUTTON_MASK) {
                _input.mi.mouseData = WHEEL_DELTA;
            } else if (state->buttons & VD_AGENT_DBUTTON_MASK) {
                _input.mi.mouseData = (DWORD)(-WHEEL_DELTA);
            }
        }
        _buttons_state = state->buttons;
    }

    _input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | mouse_move |
                        mouse_wheel | buttons_change;

    if ((mouse_move && GetTickCount() - _input_time > VD_INPUT_INTERVAL_MS) || buttons_change ||
                                                                                     mouse_wheel) {
        ret = send_input();
    } else if (!_pending_input) {
        if (SetTimer(_hwnd, VD_TIMER_ID, VD_INPUT_INTERVAL_MS, NULL)) {
            _pending_input = true;
        } else {
            vd_printf("SetTimer failed: %lu", GetLastError());
            _running = false;
            ret = false;
        }
    }
    _desktop_layout->unlock();
    return ret;
}

bool VDAgent::handle_mon_config(VDAgentMonitorsConfig* mon_config, uint32_t port)
{
    VDIChunk* reply_chunk;
    VDAgentMessage* reply_msg;
    VDAgentReply* reply;
    size_t display_count;

    display_count = _desktop_layout->get_display_count();
    for (uint32_t i = 0; i < display_count; i++) {
        DisplayMode* mode = _desktop_layout->get_display(i);
        ASSERT(mode);
        if (i >= mon_config->num_of_monitors) {
            vd_printf("%d. detached", i);
            mode->set_attached(false);
            continue;
        }
        VDAgentMonConfig* mon = &mon_config->monitors[i];
        vd_printf("%d. %u*%u*%u (%d,%d) %u", i, mon->width, mon->height, mon->depth, mon->x,
                  mon->y, !!(mon_config->flags & VD_AGENT_CONFIG_MONITORS_FLAG_USE_POS));
        mode->set_res(mon->width, mon->height, mon->depth);
        if (mon_config->flags & VD_AGENT_CONFIG_MONITORS_FLAG_USE_POS) {
            mode->set_pos(mon->x, mon->y);
        }
        mode->set_attached(true);
    }
    if (display_count) {
        _desktop_layout->set_displays();
    }

    DWORD msg_size = VD_MESSAGE_HEADER_SIZE + sizeof(VDAgentReply);
    reply_chunk = new_chunk(msg_size);
    if (!reply_chunk) {
        return false;
    }
    reply_chunk->hdr.port = port;
    reply_chunk->hdr.size = sizeof(VDAgentMessage) + sizeof(VDAgentReply);
    reply_msg = (VDAgentMessage*)reply_chunk->data;
    reply_msg->protocol = VD_AGENT_PROTOCOL;
    reply_msg->type = VD_AGENT_REPLY;
    reply_msg->opaque = 0;
    reply_msg->size = sizeof(VDAgentReply);
    reply = (VDAgentReply*)reply_msg->data;
    reply->type = VD_AGENT_MONITORS_CONFIG;
    reply->error = display_count ? VD_AGENT_SUCCESS : VD_AGENT_ERROR;
    enqueue_chunk(reply_chunk);
    return true;
}

bool VDAgent::handle_clipboard(VDAgentClipboard* clipboard, uint32_t size)
{
    HANDLE clip_data;
    UINT format;
    bool ret = false;

    if (_clipboard_owner != owner_client) {
        vd_printf("Received clipboard data from client while clipboard is not owned by client");
        SetEvent(_clipboard_event);
        return false;
    }
    if (clipboard->type == VD_AGENT_CLIPBOARD_NONE) {
        SetEvent(_clipboard_event);
        return false;
    }
    switch (clipboard->type) {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT:
        clip_data = utf8_alloc((LPCSTR)clipboard->data, size);
        break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG:
    case VD_AGENT_CLIPBOARD_IMAGE_BMP: {
        DWORD cximage_format = get_cximage_format(clipboard->type);
        ASSERT(cximage_format);
        CxImage image(clipboard->data, size, cximage_format);
        clip_data = image.CopyToHandle();
        break;
    }
    default:
        vd_printf("Unsupported clipboard type %u", clipboard->type);
        return true;
    }
    format = get_clipboard_format(clipboard->type);
    if (SetClipboardData(format, clip_data)) {
        SetEvent(_clipboard_event);
        return true;
    }
    // We retry clipboard open-empty-set-close only when there is a timeout in on_clipboard_request()
    if (!OpenClipboard(_hwnd)) {
        return false;
    }
    EmptyClipboard();
    ret = !!SetClipboardData(format, clip_data);
    CloseClipboard();
    return ret;
}

HGLOBAL VDAgent::utf8_alloc(LPCSTR data, int size)
{
    HGLOBAL handle;
    LPVOID buf;
    int len;

    // Received utf8 string is not null-terminated
    if (!(len = MultiByteToWideChar(CP_UTF8, 0, data, size, NULL, 0))) {
        return NULL;
    }
    len++;
    // Allocate and lock clipboard memory
    if (!(handle = GlobalAlloc(GMEM_DDESHARE, len * sizeof(WCHAR)))) {
        return NULL;
    }
    if (!(buf = GlobalLock(handle))) {
        GlobalFree(handle);
        return NULL;
    }
    // Translate data and set clipboard content
    if (!(MultiByteToWideChar(CP_UTF8, 0, data, size, (LPWSTR)buf, len))) {
        GlobalUnlock(handle);
        GlobalFree(handle);
        return NULL;
    }
    ((LPWSTR)buf)[len - 1] = L'\0';
    GlobalUnlock(handle);
    return handle;
}

void VDAgent::set_display_depth(uint32_t depth)
{
    size_t display_count;

    display_count = _desktop_layout->get_display_count();

    // setting depth for all the monitors, including unattached ones
    for (uint32_t i = 0; i < display_count; i++) {
        DisplayMode* mode = _desktop_layout->get_display(i);
        ASSERT(mode);
        mode->set_depth(depth);
    }

    if (display_count) {
        _desktop_layout->set_displays();
    }
}

void VDAgent::load_display_setting()
{
    _display_setting.load();
}

bool VDAgent::send_announce_capabilities(bool request)
{
    DWORD msg_size;
    VDIChunk* caps_chunk;
    VDAgentMessage* caps_msg;
    VDAgentAnnounceCapabilities* caps;
    uint32_t caps_size;
    uint32_t internal_msg_size = sizeof(VDAgentAnnounceCapabilities) + VD_AGENT_CAPS_BYTES;

    msg_size = VD_MESSAGE_HEADER_SIZE + internal_msg_size;
    caps_chunk = new_chunk(msg_size);
    if (!caps_chunk) {
        return false;
    }
    caps_size = VD_AGENT_CAPS_SIZE;
    caps_chunk->hdr.port = VDP_CLIENT_PORT;
    caps_chunk->hdr.size = sizeof(VDAgentMessage) + internal_msg_size;
    caps_msg = (VDAgentMessage*)caps_chunk->data;
    caps_msg->protocol = VD_AGENT_PROTOCOL;
    caps_msg->type = VD_AGENT_ANNOUNCE_CAPABILITIES;
    caps_msg->opaque = 0;
    caps_msg->size = internal_msg_size;
    caps = (VDAgentAnnounceCapabilities*)caps_msg->data;
    caps->request = request;
    memset(caps->caps, 0, VD_AGENT_CAPS_BYTES);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MOUSE_STATE);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_REPLY);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_DISPLAY_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    vd_printf("Sending capabilities:");
    for (uint32_t i = 0 ; i < caps_size; ++i) {
        vd_printf("%X", caps->caps[i]);
    }
    enqueue_chunk(caps_chunk);
    return true;
}

bool VDAgent::handle_announce_capabilities(VDAgentAnnounceCapabilities* announce_capabilities,
                                           uint32_t msg_size)
{
    uint32_t caps_size = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg_size);

    vd_printf("Got capabilities (%d)", caps_size);
    for (uint32_t i = 0 ; i < caps_size; ++i) {
        vd_printf("%X", announce_capabilities->caps[i]);
    }
    if (caps_size != _client_caps_size) {
        delete[] _client_caps;
        _client_caps = new uint32_t[caps_size];
        ASSERT(_client_caps != NULL);
        _client_caps_size = caps_size;
    }
    memcpy(_client_caps, announce_capabilities->caps, sizeof(_client_caps[0]) * caps_size);
    if (announce_capabilities->request) {
        return send_announce_capabilities(false);
    }
    return true;
}

bool VDAgent::handle_display_config(VDAgentDisplayConfig* display_config, uint32_t port)
{
    DisplaySettingOptions disp_setting_opts;
    VDIChunk* reply_chunk;
    VDAgentMessage* reply_msg;
    VDAgentReply* reply;
    DWORD msg_size;

    if (display_config->flags & VD_AGENT_DISPLAY_CONFIG_FLAG_DISABLE_WALLPAPER) {
        disp_setting_opts._disable_wallpaper = TRUE;
    }

    if (display_config->flags & VD_AGENT_DISPLAY_CONFIG_FLAG_DISABLE_FONT_SMOOTH) {
       disp_setting_opts._disable_font_smoothing = TRUE;
    }

    if (display_config->flags & VD_AGENT_DISPLAY_CONFIG_FLAG_DISABLE_ANIMATION) {
        disp_setting_opts._disable_animation = TRUE;
    }

    _display_setting.set(disp_setting_opts);

    if (display_config->flags & VD_AGENT_DISPLAY_CONFIG_FLAG_SET_COLOR_DEPTH) {
        set_display_depth(display_config->depth);
    }

    msg_size = VD_MESSAGE_HEADER_SIZE + sizeof(VDAgentReply);
    reply_chunk = new_chunk(msg_size);
    if (!reply_chunk) {
        return false;
    }
    reply_chunk->hdr.port = port;
    reply_chunk->hdr.size = sizeof(VDAgentMessage) + sizeof(VDAgentReply);
    reply_msg = (VDAgentMessage*)reply_chunk->data;
    reply_msg->protocol = VD_AGENT_PROTOCOL;
    reply_msg->type = VD_AGENT_REPLY;
    reply_msg->opaque = 0;
    reply_msg->size = sizeof(VDAgentReply);
    reply = (VDAgentReply*)reply_msg->data;
    reply->type = VD_AGENT_DISPLAY_CONFIG;
    reply->error = VD_AGENT_SUCCESS;
    enqueue_chunk(reply_chunk);
    return true;
}

#define MIN(a, b) ((a) > (b) ? (b) : (a))

bool VDAgent::write_clipboard(VDAgentMessage* msg, uint32_t size)
{
    uint32_t pos = 0;
    bool ret = true;

    ASSERT(msg && size);
    //FIXME: do it smarter - no loop, no memcopy
    MUTEX_LOCK(_message_mutex);
    while (pos < size) {
        DWORD n = MIN(sizeof(VDIChunk) + size - pos, VD_AGENT_MAX_DATA_SIZE);
        VDIChunk* chunk = new_chunk(n);
        if (!chunk) {
            ret = false;
            break;
        }
        chunk->hdr.port = VDP_CLIENT_PORT;
        chunk->hdr.size = n - sizeof(VDIChunk);
        memcpy(chunk->data, (char*)msg + pos, n - sizeof(VDIChunk));
        enqueue_chunk(chunk);
        pos += (n - sizeof(VDIChunk));
    }
    MUTEX_UNLOCK(_message_mutex);
    return ret;
}

bool VDAgent::write_message(uint32_t type, uint32_t size = 0, void* data = NULL)
{
    VDIChunk* chunk;
    VDAgentMessage* msg;

    chunk = new_chunk(VD_MESSAGE_HEADER_SIZE + size);
    if (!chunk) {
        return false;
    }
    chunk->hdr.port = VDP_CLIENT_PORT;
    chunk->hdr.size = sizeof(VDAgentMessage) + size;
    msg = (VDAgentMessage*)chunk->data;
    msg->protocol = VD_AGENT_PROTOCOL;
    msg->type = type;
    msg->opaque = 0;
    msg->size = size;
    if (size && data) {
        memcpy(msg->data, data, size);
    }
    enqueue_chunk(chunk);
    return true;
}

void VDAgent::on_clipboard_grab()
{
    uint32_t* types = new uint32_t[clipboard_formats_count * VD_CLIPBOARD_FORMAT_MAX_TYPES];
    int count = 0;

    if (!VD_AGENT_HAS_CAPABILITY(_client_caps, _client_caps_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
        return;
    }
    for (unsigned int i = 0; i < clipboard_formats_count; i++) {
        if (IsClipboardFormatAvailable(clipboard_formats[i].format)) {
            for (uint32_t* ptype = clipboard_formats[i].types; *ptype; ptype++) {
                types[count++] = *ptype;
            }
        }
    }
    if (count) {
        write_message(VD_AGENT_CLIPBOARD_GRAB, count * sizeof(types[0]), types);
        set_clipboard_owner(owner_guest);
    } else {
        vd_printf("Unsupported clipboard format");       
    }  
    delete[] types;
}

// In delayed rendering, Windows requires us to SetClipboardData before we return from
// handling WM_RENDERFORMAT. Therefore, we try our best by sending CLIPBOARD_REQUEST to the
// agent, while waiting alertably for a while (hoping for good) for receiving CLIPBOARD data
// or CLIPBOARD_RELEASE from the agent, which both will signal clipboard_event.
// In case of unsupported format, wrong clipboard owner or no clipboard capability, we do nothing in
// WM_RENDERFORMAT and return immediately.
// FIXME: need to be handled using request queue
void VDAgent::on_clipboard_request(UINT format)
{
    uint32_t type;

    if (_clipboard_owner != owner_client) {
        vd_printf("Received render request event for format %u"
                  "while clipboard is not owned by client", format);
        return;
    }
    if (!(type = get_clipboard_type(format))) {
        vd_printf("Unsupported clipboard format %u", format);
        return;
    }
    if (!VD_AGENT_HAS_CAPABILITY(_client_caps, _client_caps_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
        return;
    }

    VDAgentClipboardRequest request = {type};
    if (!write_message(VD_AGENT_CLIPBOARD_REQUEST, sizeof(request), &request)) {
        return;
    }

    // next clipboard event will be considered a reply to this request
    ResetEvent(_clipboard_event);

    DWORD start_tick = GetTickCount();
    do {
        DWORD wait_result = WaitForSingleObjectEx(_clipboard_event, 1000, TRUE);

        switch (wait_result) {
        case WAIT_OBJECT_0:
            return;
        case WAIT_IO_COMPLETION:
        case WAIT_TIMEOUT:
            break;
        default:
            vd_printf("Wait error (%lu)\n", GetLastError());
            return;
        }
    } while (GetTickCount() < start_tick + VD_CLIPBOARD_TIMEOUT_MS);

    vd_printf("wait timeout.. ");
}

void VDAgent::on_clipboard_release()
{
    if (!VD_AGENT_HAS_CAPABILITY(_client_caps, _client_caps_size,
                                 VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
        return;
    }
    if (_clipboard_owner == owner_guest) {
        write_message(VD_AGENT_CLIPBOARD_RELEASE, 0, NULL);
    }
}

bool VDAgent::handle_clipboard_grab(VDAgentClipboardGrab* clipboard_grab, uint32_t size)
{
    std::set<uint32_t> grab_formats;

    _grab_types.clear();
    for (uint32_t i = 0; i < size / sizeof(clipboard_grab->types[0]); i++) {
        vd_printf("grab type %u", clipboard_grab->types[i]);
        uint32_t format = get_clipboard_format(clipboard_grab->types[i]);
        //On first supported type, open and empty the clipboard
        if (format && grab_formats.empty()) {
            if (!OpenClipboard(_hwnd)) {
                return false;
            }
            EmptyClipboard();
        }
        //For all supported type set delayed rendering
        if (format) {
            _grab_types.insert(clipboard_grab->types[i]);
            if (grab_formats.insert(format).second) {
                SetClipboardData(format, NULL);
            }
        }
    }
    if (grab_formats.empty()) {
        vd_printf("No supported clipboard types in client grab");
        return true;
    }
    CloseClipboard();
    set_clipboard_owner(owner_client);
    return true;
}

// If handle_clipboard_request() fails, its caller sends VD_AGENT_CLIPBOARD message with type
// VD_AGENT_CLIPBOARD_NONE and no data, so the client will know the request failed.
bool VDAgent::handle_clipboard_request(VDAgentClipboardRequest* clipboard_request)
{
    VDAgentMessage* msg;
    uint32_t msg_size;
    UINT format;
    HANDLE clip_data;
    uint8_t* new_data = NULL;
    long new_size;
    size_t len = 0;
    CxImage image;

    if (_clipboard_owner != owner_guest) {
        vd_printf("Received clipboard request from client while clipboard is not owned by guest");
        return false;
    }
    if (!(format = get_clipboard_format(clipboard_request->type))) {
        vd_printf("Unsupported clipboard type %u", clipboard_request->type);
        return false;
    }
    if (!IsClipboardFormatAvailable(format) || !OpenClipboard(_hwnd)) {
        return false;
    }
    if (!(clip_data = GetClipboardData(format))) {
        CloseClipboard();
        return false;
    }
    switch (clipboard_request->type) {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT:
        if (!(new_data = (uint8_t*)GlobalLock(clip_data))) {
            break;
        }
        len = wcslen((LPCWSTR)new_data);
        new_size = WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)new_data, (int)len, NULL, 0, NULL, NULL);
        break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG:
    case VD_AGENT_CLIPBOARD_IMAGE_BMP: {
        DWORD cximage_format = get_cximage_format(clipboard_request->type);
        ASSERT(cximage_format);
        if (!image.CreateFromHANDLE(clip_data)) {
            vd_printf("Image create from handle failed");
            break;
        }
        if (!image.Encode(new_data, new_size, cximage_format)) {
            vd_printf("Image encode to type %u failed", clipboard_request->type);
            break;
        }
        vd_printf("Image encoded to %lu bytes", new_size);
        break;
    }
    }
    if (!new_size) {
        CloseClipboard();
        return false;
    }
    msg_size = sizeof(VDAgentMessage) + sizeof(VDAgentClipboard) + new_size;
    msg = (VDAgentMessage*)new uint8_t[msg_size];
    msg->protocol = VD_AGENT_PROTOCOL;
    msg->type = VD_AGENT_CLIPBOARD;
    msg->opaque = 0;
    msg->size = (uint32_t)(sizeof(VDAgentClipboard) + new_size);
    VDAgentClipboard* clipboard = (VDAgentClipboard*)msg->data;
    clipboard->type = clipboard_request->type;

    switch (clipboard_request->type) {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT:
        WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)new_data, (int)len, (LPSTR)clipboard->data,
                            new_size, NULL, NULL);
        GlobalUnlock(clip_data);
        break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG:
    case VD_AGENT_CLIPBOARD_IMAGE_BMP:
        memcpy(clipboard->data, new_data, new_size);
        image.FreeMemory(new_data);
        break;
    }
    CloseClipboard();
    write_clipboard(msg, msg_size);
    delete[] (uint8_t *)msg;
    return true;
}

void VDAgent::handle_clipboard_release()
{
    if (_clipboard_owner != owner_client) {
        vd_printf("Received clipboard release from client while clipboard is not owned by client");
        return;
    }
    SetEvent(_clipboard_event);
    set_clipboard_owner(owner_none);
}

uint32_t VDAgent::get_clipboard_format(uint32_t type)
{
    for (unsigned int i = 0; i < clipboard_formats_count; i++) {
        for (uint32_t* ptype = clipboard_formats[i].types; *ptype; ptype++) {
            if (*ptype == type) {
                return clipboard_formats[i].format;
            }
        }
    }
    return 0;
}

uint32_t VDAgent::get_clipboard_type(uint32_t format)
{
    uint32_t* types = NULL;

    for (unsigned int i = 0; i < clipboard_formats_count && !types; i++) {
        if (clipboard_formats[i].format == format) {
            types = clipboard_formats[i].types;
        }
    }
    if (!types) {
        return 0;
    }
    for (uint32_t* ptype = types; *ptype; ptype++) {
        if (_grab_types.find(*ptype) != _grab_types.end()) {
            return *ptype;
        }
    }
    return 0;
}

DWORD VDAgent::get_cximage_format(uint32_t type)
{
    for (unsigned int i = 0; i < sizeof(image_types) / sizeof(image_types[0]); i++) {
        if (image_types[i].type == type) {
            return image_types[i].cximage_format;
        }
    }
    return 0;
}

void VDAgent::set_clipboard_owner(int new_owner)
{
    // FIXME: Clear requests, clipboard data and state
    if (new_owner == owner_none) {
        on_clipboard_release();
    }
    _clipboard_owner = new_owner;
}

VDIPort *create_virtio_vdi_port()
{
    return new VirtioVDIPort();
}

VDIPort *create_pci_vdi_port()
{
    return new PCIVDIPort();
}

bool VDAgent::init_vdi_port()
{
    VDIPort* (*creators[])(void) = { create_virtio_vdi_port, create_pci_vdi_port };

    for (unsigned int i = 0 ; i < sizeof(creators)/sizeof(creators[0]); ++i) {
        _vdi_port = creators[i]();
        if (_vdi_port->init()) {
            return true;
        }
        delete _vdi_port;
    }
    _vdi_port = NULL;
    return false;
}

void VDAgent::dispatch_message(VDAgentMessage* msg, uint32_t port)
{
    VDAgent* a = _singleton;
    bool res = true;

    switch (msg->type) {
    case VD_AGENT_MOUSE_STATE:
        res = a->handle_mouse_event((VDAgentMouseState*)msg->data);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        res = a->handle_mon_config((VDAgentMonitorsConfig*)msg->data, port);
        break;
    case VD_AGENT_CLIPBOARD:
        a->handle_clipboard((VDAgentClipboard*)msg->data, msg->size - sizeof(VDAgentClipboard));
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        a->handle_clipboard_grab((VDAgentClipboardGrab*)msg->data, msg->size);        
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        res = a->handle_clipboard_request((VDAgentClipboardRequest*)msg->data);
        if (!res) {
            VDAgentClipboard clipboard = {VD_AGENT_CLIPBOARD_NONE};
            res = a->write_message(VD_AGENT_CLIPBOARD, sizeof(clipboard), &clipboard);
        }
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        a->handle_clipboard_release();
        break;
    case VD_AGENT_DISPLAY_CONFIG:
        res = a->handle_display_config((VDAgentDisplayConfig*)msg->data, port);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        res = a->handle_announce_capabilities((VDAgentAnnounceCapabilities*)msg->data, msg->size);
        break;
    default:
        vd_printf("Unsupported message type %u size %u", msg->type, msg->size);
    }
    if (!res) {
        vd_printf("handling message type %u failed: %lu", msg->type, GetLastError());
        a->_running = false;
    }
}

void VDAgent::handle_port_in()
{
    static char buf[sizeof(VDIChunk) + VD_AGENT_MAX_DATA_SIZE] = {0, };
    VDIChunk* chunk = (VDIChunk*)buf;
    uint32_t chunk_size;

    while (_running)  {
        if (!chunk->hdr.size && _vdi_port->read_ring_size() >= sizeof(VDIChunk)) {
            if (_vdi_port->ring_read(chunk, sizeof(VDIChunk)) != sizeof(VDIChunk)) {
                vd_printf("ring_read of chunk header failed");
                _running = false;
                break;
            }
            if (sizeof(VDIChunk) + chunk->hdr.size > sizeof(buf)) {
                vd_printf("chunk is too large, size %u port %u", chunk->hdr.size, chunk->hdr.port);
                _running = false;
                break;
            }
        }
        chunk_size = chunk->hdr.size;
        if (!chunk_size || _vdi_port->read_ring_size() < chunk_size) {
            break;
        }
        if (_vdi_port->ring_read(chunk->data, chunk_size) != chunk_size) {
            vd_printf("ring_read of chunk data failed");
            _running = false;
            break;
        }
        handle_chunk(chunk);
        chunk->hdr.size = 0;
   }
}

void VDAgent::handle_chunk(VDIChunk* chunk)
{
    //FIXME: currently assumes that multi-part msg arrives only from client port
    if (_in_msg_pos == 0 || chunk->hdr.port == VDP_SERVER_PORT) {
        if (chunk->hdr.size < sizeof(VDAgentMessage)) {
            return;
        }
        VDAgentMessage* msg = (VDAgentMessage*)chunk->data;
        if (msg->protocol != VD_AGENT_PROTOCOL) {
            vd_printf("Invalid protocol %u", msg->protocol);
            _running = false;
            return;
        }
        uint32_t msg_size = sizeof(VDAgentMessage) + msg->size;
        if (chunk->hdr.size == msg_size) {
            dispatch_message(msg, chunk->hdr.port);
        } else {
            ASSERT(chunk->hdr.size < msg_size);
            _in_msg = (VDAgentMessage*)new uint8_t[msg_size];
            memcpy(_in_msg, chunk->data, chunk->hdr.size);
            _in_msg_pos = chunk->hdr.size;
        }
    } else {
        memcpy((uint8_t*)_in_msg + _in_msg_pos, chunk->data, chunk->hdr.size);
        _in_msg_pos += chunk->hdr.size;
        if (_in_msg_pos == sizeof(VDAgentMessage) + _in_msg->size) {
            dispatch_message(_in_msg, 0);
            _in_msg_pos = 0;
            delete[] (uint8_t *)_in_msg;
            _in_msg = NULL;
        }
    }
}

void VDAgent::handle_port_out()
{
    MUTEX_LOCK(_message_mutex);
    while (_running && !_message_queue.empty()) {
        VDIChunk* chunk = _message_queue.front();
        DWORD size = sizeof(VDIChunk) + chunk->hdr.size;

        if (size > _vdi_port->write_ring_free_space()) {
            break;
        }
        _message_queue.pop();
        if (_vdi_port->ring_write(chunk, size) != size) {
            vd_printf("ring_write failed");
            _running = false;
            return;
        }
        delete chunk;
    }
    MUTEX_UNLOCK(_message_mutex);
}

VDIChunk* VDAgent::new_chunk(DWORD bytes)
{
    return (VDIChunk*)(new char[bytes]);
}

void VDAgent::enqueue_chunk(VDIChunk* chunk)
{
    MUTEX_LOCK(_message_mutex);
    _message_queue.push(chunk);
    MUTEX_UNLOCK(_message_mutex);
    handle_port_out();
}

LRESULT CALLBACK VDAgent::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    VDAgent* a = _singleton;

    switch (message) {
    case WM_DISPLAYCHANGE:
        vd_printf("Display change");
        a->_desktop_layout->get_displays();
        break;
    case WM_TIMER:
        a->send_input();
        break;
    case WM_CHANGECBCHAIN:
        if (a->_hwnd_next_viewer == (HWND)wparam) {
            a->_hwnd_next_viewer = (HWND)lparam;
        } else if (a->_hwnd_next_viewer) {
            SendMessage(a->_hwnd_next_viewer, message, wparam, lparam);
        }
        break;
    case WM_DRAWCLIPBOARD:
        if (a->_hwnd != GetClipboardOwner()) {
            a->set_clipboard_owner(a->owner_none);
            a->on_clipboard_grab();
        }
        if (a->_hwnd_next_viewer) {
            SendMessage(a->_hwnd_next_viewer, message, wparam, lparam);
        }
        break;
    case WM_RENDERFORMAT:
        a->on_clipboard_request((UINT)wparam);
        break;
    case WM_ENDSESSION:
        if (wparam) {
            vd_printf("Session ended");
            if (a->_clipboard_owner == owner_guest) {
                a->set_clipboard_owner(owner_none);
            }
            a->set_control_event(CONTROL_STOP);
        }
        break;
    case WM_WTSSESSION_CHANGE:
        if (wparam == WTS_SESSION_LOGON) {
            a->set_control_event(CONTROL_LOGON);
        }
        break;
    default:
        return DefWindowProc(hwnd, message, wparam, lparam);
    }
    return 0;
}

#ifdef __GNUC__
int main(int argc,char **argv)
#else
int APIENTRY _tWinMain(HINSTANCE instance, HINSTANCE prev_instance, LPTSTR cmd_line, int cmd_show)
#endif
{
    VDAgent* vdagent = VDAgent::get();
    vdagent->run();
    delete vdagent;
    return 0;
}

