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
#include "desktop_layout.h"
#include "display_setting.h"
#include "file_xfer.h"
#include "ximage.h"
#undef max
#undef min
#include <spice/macros.h>
#include <wtsapi32.h>
#include <lmcons.h>
#include <queue>
#include <set>
#include <vector>

#define VD_AGENT_LOG_PATH       TEXT("%svdagent.log")
#define VD_AGENT_WINCLASS_NAME  TEXT("VDAGENT")
#define VD_INPUT_INTERVAL_MS    20
#define VD_TIMER_ID             1
#define VD_CLIPBOARD_TIMEOUT_MS 3000
#define VD_CLIPBOARD_FORMAT_MAX_TYPES 16

// only in vista+, not yet in mingw
#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE      0x031D
#endif

//FIXME: extract format/type stuff to win_vdagent_common for use by windows\platform.cpp as well
typedef struct VDClipboardFormat {
    uint32_t format;
    uint32_t types[VD_CLIPBOARD_FORMAT_MAX_TYPES];
} VDClipboardFormat;

static const VDClipboardFormat clipboard_formats[] = {
    {CF_UNICODETEXT, {VD_AGENT_CLIPBOARD_UTF8_TEXT, 0}},
    //FIXME: support more image types
    {CF_DIB, {VD_AGENT_CLIPBOARD_IMAGE_PNG, VD_AGENT_CLIPBOARD_IMAGE_BMP, 0}},
};

#define clipboard_formats_count SPICE_N_ELEMENTS(clipboard_formats)

typedef struct ImageType {
    uint32_t type;
    DWORD cximage_format;
} ImageType;

static const ImageType image_types[] = {
    {VD_AGENT_CLIPBOARD_IMAGE_PNG, CXIMAGE_FORMAT_PNG},
    {VD_AGENT_CLIPBOARD_IMAGE_BMP, CXIMAGE_FORMAT_BMP},
};

typedef struct ALIGN_VC VDIChunk {
    VDIChunkHeader hdr;
    uint8_t data[0];
} ALIGN_GCC VDIChunk;

#define VD_MESSAGE_HEADER_SIZE (sizeof(VDIChunk) + sizeof(VDAgentMessage))
#define VD_READ_BUF_SIZE       (sizeof(VDIChunk) + VD_AGENT_MAX_DATA_SIZE)

typedef BOOL (WINAPI *PCLIPBOARD_OP)(HWND);

class VDAgent {
public:
    static VDAgent* get();
    ~VDAgent();
    bool run();

private:
    VDAgent();
    void input_desktop_message_loop();
    void event_dispatcher(DWORD timeout, DWORD wake_mask);
    bool handle_mouse_event(VDAgentMouseState* state);
    bool handle_announce_capabilities(VDAgentAnnounceCapabilities* announce_capabilities,
                                      uint32_t msg_size);
    bool handle_mon_config(VDAgentMonitorsConfig* mon_config, uint32_t port);
    bool handle_clipboard(VDAgentClipboard* clipboard, uint32_t size);
    bool handle_clipboard_grab(VDAgentClipboardGrab* clipboard_grab, uint32_t size);
    bool handle_clipboard_request(VDAgentClipboardRequest* clipboard_request);
    void handle_clipboard_release();
    bool handle_display_config(VDAgentDisplayConfig* display_config, uint32_t port);
    bool handle_max_clipboard(VDAgentMaxClipboard *msg, uint32_t size);
    void handle_chunk(VDIChunk* chunk);
    void on_clipboard_grab();
    void on_clipboard_request(UINT format);
    void on_clipboard_release();
    DWORD get_buttons_change(DWORD last_buttons_state, DWORD new_buttons_state,
                             DWORD mask, DWORD down_flag, DWORD up_flag);
    static HGLOBAL utf8_alloc(LPCSTR data, int size);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static DWORD WINAPI event_thread_proc(LPVOID param);
    static VOID CALLBACK read_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlapped);
    static VOID CALLBACK write_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlapped);
    void dispatch_message(VDAgentMessage* msg, uint32_t port);
    uint32_t get_clipboard_format(uint32_t type) const;
    uint32_t get_clipboard_type(uint32_t format) const;
    DWORD get_cximage_format(uint32_t type) const;
    enum { owner_none, owner_guest, owner_client };
    void set_clipboard_owner(int new_owner);
    enum { CONTROL_STOP, CONTROL_RESET, CONTROL_DESKTOP_SWITCH, CONTROL_LOGON, CONTROL_CLIPBOARD };
    void set_control_event(int control_command);
    void handle_control_event();
    VDIChunk* new_chunk(DWORD bytes = 0);
    void enqueue_chunk(VDIChunk* msg);
    bool write_message(uint32_t type, uint32_t size, void* data);
    bool write_clipboard(VDAgentMessage* msg, uint32_t size);
    bool init_vio_serial();
    bool send_input();
    void set_display_depth(uint32_t depth);
    void load_display_setting();
    bool send_announce_capabilities(bool request);
    void cleanup_in_msg();
    void cleanup();
    bool has_capability(unsigned int capability) const {
        return VD_AGENT_HAS_CAPABILITY(_client_caps.begin(), _client_caps.size(),
                                       capability);
    }

private:
    static VDAgent* _singleton;
    HWND _hwnd;
    HWND _hwnd_next_viewer;
    HMODULE _user_lib;
    PCLIPBOARD_OP _add_clipboard_listener;
    PCLIPBOARD_OP _remove_clipboard_listener;
    int _system_version;
    int _clipboard_owner;
    DWORD _clipboard_tick;
    DWORD _buttons_state;
    ULONG _mouse_x;
    ULONG _mouse_y;
    INPUT _input;
    DWORD _input_time;
    HANDLE _control_event;
    HANDLE _stop_event;
    VDAgentMessage* _in_msg;
    uint32_t _in_msg_pos;
    bool _pending_input;
    bool _running;
    bool _session_is_locked;
    bool _desktop_switch;
    DesktopLayout* _desktop_layout;
    bool _updating_display_config;
    DisplaySetting _display_setting;
    FileXfer _file_xfer;
    HANDLE _vio_serial;
    OVERLAPPED _read_overlapped;
    OVERLAPPED _write_overlapped;
    CHAR _read_buf[VD_READ_BUF_SIZE];
    DWORD _read_pos;
    DWORD _write_pos;
    mutex_t _control_mutex;
    mutex_t _message_mutex;
    std::queue<int> _control_queue;
    std::queue<VDIChunk*> _message_queue;

    bool _logon_desktop;
    bool _display_setting_initialized;
    bool _logon_occured;

    int32_t _max_clipboard;
    std::vector<uint32_t> _client_caps;

    std::set<uint32_t> _grab_types;

    VDLog* _log;
};

VDAgent* VDAgent::_singleton = NULL;

#define VIOSERIAL_PORT_PATH L"\\\\.\\Global\\com.redhat.spice.0"

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
    , _user_lib (NULL)
    , _add_clipboard_listener (NULL)
    , _remove_clipboard_listener (NULL)
    , _clipboard_owner (owner_none)
    , _clipboard_tick (0)
    , _buttons_state (0)
    , _mouse_x (0)
    , _mouse_y (0)
    , _input_time (0)
    , _control_event (NULL)
    , _stop_event (NULL)
    , _in_msg (NULL)
    , _in_msg_pos (0)
    , _pending_input (false)
    , _running (false)
    , _session_is_locked (false)
    , _desktop_switch (false)
    , _desktop_layout (NULL)
    , _display_setting (VD_AGENT_REGISTRY_KEY)
    , _vio_serial (NULL)
    , _read_pos (0)
    , _write_pos (0)
    , _logon_desktop (false)
    , _display_setting_initialized (false)
    , _max_clipboard (-1)
    , _log (NULL)
{
    TCHAR log_path[MAX_PATH];
    TCHAR temp_path[MAX_PATH];

    _system_version = supported_system_version();
    if (GetTempPath(MAX_PATH, temp_path)) {
        swprintf_s(log_path, MAX_PATH, VD_AGENT_LOG_PATH, temp_path);
        _log = VDLog::get(log_path);
    }
    ZeroMemory(&_input, sizeof(_input));
    ZeroMemory(&_read_overlapped, sizeof(_read_overlapped));
    ZeroMemory(&_write_overlapped, sizeof(_write_overlapped));
    ZeroMemory(_read_buf, sizeof(_read_buf));

    _singleton = this;
}

VDAgent::~VDAgent()
{
    delete _log;
}

DWORD WINAPI VDAgent::event_thread_proc(LPVOID param)
{
    VDAgent *agent = static_cast<VDAgent *>(param);
    HANDLE desktop_event = OpenEvent(SYNCHRONIZE, FALSE, L"WinSta0_DesktopSwitch");
    if (!desktop_event) {
        vd_printf("OpenEvent() failed: %lu", GetLastError());
        return 1;
    }
    while (agent->_running) {
        DWORD wait_ret = WaitForSingleObject(desktop_event, INFINITE);
        switch (wait_ret) {
        case WAIT_OBJECT_0:
            agent->set_control_event(CONTROL_DESKTOP_SWITCH);
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
    if (_system_version == SYS_VER_WIN_7_CLASS) {
        _user_lib = LoadLibrary(L"User32.dll");
        if (!_user_lib) {
            vd_printf("LoadLibrary failed %lu", GetLastError());
            return false;
        }
        _add_clipboard_listener =
            (PCLIPBOARD_OP)GetProcAddress(_user_lib, "AddClipboardFormatListener");
        _remove_clipboard_listener =
            (PCLIPBOARD_OP)GetProcAddress(_user_lib, "RemoveClipboardFormatListener");
        if (!_add_clipboard_listener || !_remove_clipboard_listener) {
            vd_printf("GetProcAddress failed %lu", GetLastError());
            cleanup();
            return false;
        }
    }
    _control_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!_control_event) {
        vd_printf("CreateEvent() failed: %lu", GetLastError());
        cleanup();
        return false;
    }
    _stop_event = OpenEvent(SYNCHRONIZE, FALSE, VD_AGENT_STOP_EVENT);
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
    if (!init_vio_serial()) {
        cleanup();
        return false;
    }
    if (!ReadFileEx(_vio_serial, _read_buf, sizeof(VDIChunk), &_read_overlapped, read_completion) &&
            GetLastError() != ERROR_IO_PENDING) {
        vd_printf("vio_serial read error %lu", GetLastError());
        cleanup();
        return false;
    }
    _running = true;
    event_thread = CreateThread(NULL, 0, event_thread_proc, this, 0, &event_thread_id);
    if (!event_thread) {
        vd_printf("CreateThread() failed: %lu", GetLastError());
        cleanup();
        return false;
    }
    send_announce_capabilities(true);
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
    FreeLibrary(_user_lib);
    CloseHandle(_stop_event);
    CloseHandle(_control_event);
    CloseHandle(_vio_serial);
    delete _desktop_layout;
}

void VDAgent::set_control_event(int control_command)
{
    MutexLocker lock(_control_mutex);
    _control_queue.push(control_command);
    if (_control_event && !SetEvent(_control_event)) {
        vd_printf("SetEvent() failed: %lu", GetLastError());
    }
}

void VDAgent::handle_control_event()
{
    MutexLocker lock(_control_mutex);
    while (_control_queue.size()) {
        int control_command = _control_queue.front();
        _control_queue.pop();
        vd_printf("Control command %d", control_command);
        switch (control_command) {
        case CONTROL_RESET:
            _file_xfer.reset();
            set_control_event(CONTROL_CLIPBOARD);
            set_clipboard_owner(owner_none);
            break;
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
        case CONTROL_CLIPBOARD:
            _clipboard_tick = 0;
            break;
        default:
            vd_printf("Unsupported control command %u", control_command);
        }
    }
}

void VDAgent::input_desktop_message_loop()
{
    TCHAR desktop_name[MAX_PATH];
    HDESK hdesk;

    hdesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hdesk) {
        vd_printf("OpenInputDesktop() failed: %lu", GetLastError());
        _running = false;
        return;
    }
    if (!SetThreadDesktop(hdesk)) {
        vd_printf("SetThreadDesktop failed %lu", GetLastError());
        CloseDesktop(hdesk);
        _running = false;
        return;
    }
    if (GetUserObjectInformation(hdesk, UOI_NAME, desktop_name, sizeof(desktop_name), NULL)) {
        vd_printf("Desktop: %S", desktop_name);
    } else {
        vd_printf("GetUserObjectInformation failed %lu", GetLastError());
    }
    CloseDesktop(hdesk);

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
    if (_system_version == SYS_VER_WIN_7_CLASS) {
        _add_clipboard_listener(_hwnd);
    } else {
        _hwnd_next_viewer = SetClipboardViewer(_hwnd);
    }
    while (_running && !_desktop_switch) {
        event_dispatcher(INFINITE, QS_ALLINPUT);
    }
    _desktop_switch = false;
    if (_pending_input) {
        KillTimer(_hwnd, VD_TIMER_ID);
        _pending_input = false;
    }
    if (_system_version == SYS_VER_WIN_7_CLASS) {
        _remove_clipboard_listener(_hwnd);
    } else {
        ChangeClipboardChain(_hwnd, _hwnd_next_viewer);
    }
    WTSUnRegisterSessionNotification(_hwnd);
    DestroyWindow(_hwnd);
}

void VDAgent::event_dispatcher(DWORD timeout, DWORD wake_mask)
{
    HANDLE events[2];
    DWORD event_count = 1;
    DWORD wait_ret;
    MSG msg;
    enum {
        CONTROL_ACTION,
        STOP_ACTION,
    } actions[SPICE_N_ELEMENTS(events)], action;

    events[0] = _control_event;
    actions[0] = CONTROL_ACTION;
    if (_stop_event) {
        events[event_count] = _stop_event;
        actions[event_count] = STOP_ACTION;
        event_count++;
    }

    wait_ret = MsgWaitForMultipleObjectsEx(event_count, events, timeout, wake_mask, MWMO_ALERTABLE);
    if (wait_ret == WAIT_OBJECT_0 + event_count) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        return;
    } else if (wait_ret == WAIT_IO_COMPLETION || wait_ret == WAIT_TIMEOUT) {
        return;
    } else if (wait_ret < WAIT_OBJECT_0 || wait_ret > WAIT_OBJECT_0 + event_count) {
        vd_printf("MsgWaitForMultipleObjectsEx failed: %lu %lu", wait_ret, GetLastError());
        _running = false;
        return;
    }

    action = actions[wait_ret - WAIT_OBJECT_0];
    switch (action) {
    case CONTROL_ACTION:
        handle_control_event();
        break;
    case STOP_ACTION:
        vd_printf("received stop event");
        _running = false;
        break;
    default:
        vd_printf("action not handled (%d)", action);
        _running = false;
    }
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
        DWORD w = _desktop_layout->get_total_width();
        DWORD h = _desktop_layout->get_total_height();
        w = (w > 1) ? w-1 : 1; /* coordinates are 0..w-1, protect w==0 */
        h = (h > 1) ? h-1 : 1; /* coordinates are 0..h-1, protect h==0 */
        _mouse_x = state->x;
        _mouse_y = state->y;
        mouse_move = MOUSEEVENTF_MOVE;
        _input.mi.dx = (mode->get_pos_x() + _mouse_x) * 0xffff / w;
        _input.mi.dy = (mode->get_pos_y() + _mouse_y) * 0xffff / h;
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
    bool update_displays(false);

    _updating_display_config = true;

    display_count = _desktop_layout->get_display_count();
    for (uint32_t i = 0; i < display_count; i++) {
        DisplayMode* mode = _desktop_layout->get_display(i);
        if (!mode) {
            continue;
        }
        if (i >= mon_config->num_of_monitors) {
            vd_printf("%d. detached", i);
            mode->set_attached(false);
            update_displays = true;
            continue;
        }
        VDAgentMonConfig* mon = &mon_config->monitors[i];
        vd_printf("%d. %u*%u*%u (%d,%d) %u", i, mon->width, mon->height, mon->depth, mon->x,
                  mon->y, !!(mon_config->flags & VD_AGENT_CONFIG_MONITORS_FLAG_USE_POS));
        if (mon->height == 0 && mon->depth == 0) {
            vd_printf("%d. detaching", i);
            update_displays = mode->get_attached() ? true : update_displays;
            mode->set_attached(false);
            continue;
        }
        if (mode->get_height() != mon->height || mode->get_width() != mon->width || mode->get_depth() != mon->depth) {
            mode->set_res(mon->width, mon->height, mon->depth);
            update_displays = true;
        }
        if (mon_config->flags & VD_AGENT_CONFIG_MONITORS_FLAG_USE_POS && (mode->get_pos_x() != mon->x || mode->get_pos_y() != mon->y)) {
            mode->set_pos(mon->x, mon->y);
            update_displays = true;
        }
        if (!mode->get_attached()) {
            mode->set_attached(true);
            update_displays = true;
        }
    }
    if (update_displays) {
        _desktop_layout->set_displays();
    }

    _updating_display_config = false;
    /* refresh again, in case something else changed */
    _desktop_layout->get_displays();

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
        goto fin;
    }
    if (clipboard->type == VD_AGENT_CLIPBOARD_NONE) {
        goto fin;
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
        goto fin;
    }
    format = get_clipboard_format(clipboard->type);
    if (format == 0) {
        vd_printf("Unknown clipboard format, type %u", clipboard->type);
        goto fin;
    }
    ret = !!SetClipboardData(format, clip_data);
    if (!ret) {
        DWORD err = GetLastError();
        if (err == ERROR_NOT_ENOUGH_MEMORY) {
            vd_printf("Not enough memory to set clipboard data, size %u bytes", size);
        } else {
            vd_printf("SetClipboardData failed: %lu", err);
        }
    }
fin:
    set_control_event(CONTROL_CLIPBOARD);
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
        if (mode) {
            mode->set_depth(depth);
        }
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
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_SPARSE_MONITORS_CONFIG);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_GUEST_LINEEND_CRLF);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_MAX_CLIPBOARD);
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
    _client_caps.assign(announce_capabilities->caps, announce_capabilities->caps + caps_size);

    if (has_capability(VD_AGENT_CAP_MONITORS_CONFIG_POSITION))
        _desktop_layout->set_position_configurable(true);
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

bool VDAgent::handle_max_clipboard(VDAgentMaxClipboard *msg, uint32_t size)
{
    if (size != sizeof(VDAgentMaxClipboard)) {
        vd_printf("VDAgentMaxClipboard: unexpected msg size %u (expected %lu)",
                  size, (unsigned long)sizeof(VDAgentMaxClipboard));
        return false;
    }
    vd_printf("Set max clipboard size: %d", msg->max);
    _max_clipboard = msg->max;
    return true;
}

bool VDAgent::write_clipboard(VDAgentMessage* msg, uint32_t size)
{
    uint32_t pos = 0;
    bool ret = true;

    ASSERT(msg && size);
    //FIXME: do it smarter - no loop, no memcopy
    MutexLocker lock(_message_mutex);
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
    uint32_t types[clipboard_formats_count * VD_CLIPBOARD_FORMAT_MAX_TYPES];
    int count = 0;

    if (!has_capability(VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
        return;
    }
    if (CountClipboardFormats() == 0) {
        return;
    }
    for (unsigned int i = 0; i < clipboard_formats_count; i++) {
        if (IsClipboardFormatAvailable(clipboard_formats[i].format)) {
            for (const uint32_t* ptype = clipboard_formats[i].types; *ptype; ptype++) {
                types[count++] = *ptype;
            }
        }
    }
    if (count) {
        write_message(VD_AGENT_CLIPBOARD_GRAB, count * sizeof(types[0]), types);
        set_clipboard_owner(owner_guest);
    } else {
        UINT format = 0;
        while ((format = EnumClipboardFormats(format))) {
            vd_printf("Unsupported clipboard format %u", format);
        }
    }
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
    if (!has_capability(VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
        return;
    }

    VDAgentClipboardRequest request = {type};
    if (!write_message(VD_AGENT_CLIPBOARD_REQUEST, sizeof(request), &request)) {
        return;
    }

    _clipboard_tick = GetTickCount();
    while (_running && _clipboard_tick &&
           GetTickCount() < _clipboard_tick + VD_CLIPBOARD_TIMEOUT_MS) {
        event_dispatcher(VD_CLIPBOARD_TIMEOUT_MS, 0);
    }

    if (_clipboard_tick) {
        vd_printf("Clipboard wait timeout");
        _clipboard_tick = 0;
    } else {
        // reset incoming message state only upon completion (even after timeout)
        cleanup_in_msg();
    }
}

void VDAgent::on_clipboard_release()
{
    if (!has_capability(VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
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
        uint32_t format = get_clipboard_format(clipboard_grab->types[i]);
        vd_printf("grab type %u format=%u", clipboard_grab->types[i], format);
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
    long new_size = 0;
    size_t len = 0;
    CxImage image;
    VDAgentClipboard* clipboard = NULL;

    if (_clipboard_owner != owner_guest) {
        vd_printf("Received clipboard request from client while clipboard is not owned by guest");
        return false;
    }
    if (!(format = get_clipboard_format(clipboard_request->type))) {
        vd_printf("Unsupported clipboard type %u", clipboard_request->type);
        return false;
    }
    // on encoding only, we use HBITMAP to keep the correct palette
    if (format == CF_DIB) {
        format = CF_BITMAP;
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
        HPALETTE pal = 0;

        ASSERT(cximage_format);
        if (IsClipboardFormatAvailable(CF_PALETTE)) {
            pal = (HPALETTE)GetClipboardData(CF_PALETTE);
        }
        if (!image.CreateFromHBITMAP((HBITMAP)clip_data, pal)) {
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
        vd_printf("clipboard is empty");
        goto handle_clipboard_request_fail;
    }
    if ((_max_clipboard != -1) && (new_size > _max_clipboard)) {
        vd_printf("clipboard is too large (%ld > %d), discarding",
                  new_size, _max_clipboard);
        goto handle_clipboard_request_fail;
    }

    msg_size = sizeof(VDAgentMessage) + sizeof(VDAgentClipboard) + new_size;
    msg = (VDAgentMessage*)new uint8_t[msg_size];
    msg->protocol = VD_AGENT_PROTOCOL;
    msg->type = VD_AGENT_CLIPBOARD;
    msg->opaque = 0;
    msg->size = (uint32_t)(sizeof(VDAgentClipboard) + new_size);
    clipboard = (VDAgentClipboard*)msg->data;
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

handle_clipboard_request_fail:
    if (clipboard_request->type == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
       GlobalUnlock(clip_data);
    }
    CloseClipboard();
    return false;
}

void VDAgent::handle_clipboard_release()
{
    if (_clipboard_owner != owner_client) {
        vd_printf("Received clipboard release from client while clipboard is not owned by client");
        return;
    }
    set_control_event(CONTROL_CLIPBOARD);
    set_clipboard_owner(owner_none);
}

uint32_t VDAgent::get_clipboard_format(uint32_t type) const
{
    for (unsigned int i = 0; i < clipboard_formats_count; i++) {
        for (const uint32_t* ptype = clipboard_formats[i].types; *ptype; ptype++) {
            if (*ptype == type) {
                return clipboard_formats[i].format;
            }
        }
    }
    return 0;
}

uint32_t VDAgent::get_clipboard_type(uint32_t format) const
{
    const uint32_t* types = NULL;

    for (unsigned int i = 0; i < clipboard_formats_count && !types; i++) {
        if (clipboard_formats[i].format == format) {
            types = clipboard_formats[i].types;
        }
    }
    if (!types) {
        return 0;
    }
    for (const uint32_t* ptype = types; *ptype; ptype++) {
        if (_grab_types.find(*ptype) != _grab_types.end()) {
            return *ptype;
        }
    }
    return 0;
}

DWORD VDAgent::get_cximage_format(uint32_t type) const
{
    for (unsigned int i = 0; i < SPICE_N_ELEMENTS(image_types); i++) {
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

bool VDAgent::init_vio_serial()
{
    _vio_serial = CreateFile(VIOSERIAL_PORT_PATH, GENERIC_READ | GENERIC_WRITE , 0, NULL,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (_vio_serial == INVALID_HANDLE_VALUE) {
        vd_printf("Failed opening %ls, error %lu", VIOSERIAL_PORT_PATH, GetLastError());
        return false;
    }
    return true;
}

void VDAgent::dispatch_message(VDAgentMessage* msg, uint32_t port)
{
    bool res = true;

    switch (msg->type) {
    case VD_AGENT_MOUSE_STATE:
        res = handle_mouse_event((VDAgentMouseState*)msg->data);
        break;
    case VD_AGENT_MONITORS_CONFIG:
        res = handle_mon_config((VDAgentMonitorsConfig*)msg->data, port);
        break;
    case VD_AGENT_CLIPBOARD:
        handle_clipboard((VDAgentClipboard*)msg->data, msg->size - sizeof(VDAgentClipboard));
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        handle_clipboard_grab((VDAgentClipboardGrab*)msg->data, msg->size);
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        res = handle_clipboard_request((VDAgentClipboardRequest*)msg->data);
        if (!res) {
            VDAgentClipboard clipboard = {VD_AGENT_CLIPBOARD_NONE};
            res = write_message(VD_AGENT_CLIPBOARD, sizeof(clipboard), &clipboard);
        }
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        handle_clipboard_release();
        break;
    case VD_AGENT_DISPLAY_CONFIG:
        res = handle_display_config((VDAgentDisplayConfig*)msg->data, port);
        break;
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        res = handle_announce_capabilities((VDAgentAnnounceCapabilities*)msg->data, msg->size);
        break;
    case VD_AGENT_FILE_XFER_START: {
        VDAgentFileXferStatusMessage status;
        if (_session_is_locked) {
            VDAgentFileXferStartMessage *s = (VDAgentFileXferStartMessage *)msg->data;
            status.id = s->id;
            status.result = VD_AGENT_FILE_XFER_STATUS_ERROR;
            vd_printf("Fail to start file-xfer %u due: Locked session", status.id);
            write_message(VD_AGENT_FILE_XFER_STATUS, sizeof(status), &status);
        } else if (_file_xfer.dispatch(msg, &status)) {
            write_message(VD_AGENT_FILE_XFER_STATUS, sizeof(status), &status);
        }
        break;
    }
    case VD_AGENT_FILE_XFER_STATUS:
    case VD_AGENT_FILE_XFER_DATA: {
        VDAgentFileXferStatusMessage status;
        if (_file_xfer.dispatch(msg, &status)) {
            write_message(VD_AGENT_FILE_XFER_STATUS, sizeof(status), &status);
        }
        break;
    }
    case VD_AGENT_CLIENT_DISCONNECTED:
        vd_printf("Client disconnected, resetting agent state");
        set_control_event(CONTROL_RESET);
        break;
    case VD_AGENT_MAX_CLIPBOARD:
        res = handle_max_clipboard((VDAgentMaxClipboard*)msg->data, msg->size);
        break;
    default:
        vd_printf("Unsupported message type %u size %u", msg->type, msg->size);
    }
    if (!res) {
        vd_printf("handling message type %u failed: %lu", msg->type, GetLastError());
        _running = false;
    }
}

VOID VDAgent::read_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlapped)
{
    VDAgent* a = _singleton;
    VDIChunk* chunk = (VDIChunk*)a->_read_buf;
    DWORD count;

    if (err != 0 && err != ERROR_OPERATION_ABORTED && err != ERROR_NO_SYSTEM_RESOURCES) {
        vd_printf("vio_serial read completion error %lu", err);
        a->_running = false;
        return;
    }

    a->_read_pos += bytes;
    if (a->_read_pos < sizeof(VDIChunk)) {
        count = sizeof(VDIChunk) - a->_read_pos;
    } else if (a->_read_pos == sizeof(VDIChunk)) {
        count = chunk->hdr.size;
        if (a->_read_pos + count > sizeof(a->_read_buf)) {
            vd_printf("chunk is too large, size %u port %u", chunk->hdr.size, chunk->hdr.port);
            a->_running = false;
            return;
        }
    } else if (a->_read_pos == sizeof(VDIChunk) + chunk->hdr.size){
        a->handle_chunk(chunk);
        count = sizeof(VDIChunk);
        a->_read_pos = 0;
    } else {
        ASSERT(a->_read_pos < sizeof(VDIChunk) + chunk->hdr.size);
        count = sizeof(VDIChunk) + chunk->hdr.size - a->_read_pos;
    }

    if (!ReadFileEx(a->_vio_serial, a->_read_buf + a->_read_pos, count, overlapped,
                    read_completion) && GetLastError() != ERROR_IO_PENDING) {
        vd_printf("vio_serial read error %lu", GetLastError());
        a->_running = false;
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
        // update clipboard tick on each clipboard chunk for timeout setting
        if (_in_msg->type == VD_AGENT_CLIPBOARD && _clipboard_tick) {
            _clipboard_tick = GetTickCount();
        }
        if (_in_msg_pos == sizeof(VDAgentMessage) + _in_msg->size) {
            if (_in_msg->type == VD_AGENT_CLIPBOARD && !_clipboard_tick) {
                vd_printf("Clipboard received but dropped due to timeout");
            } else {
                dispatch_message(_in_msg, 0);
            }
            cleanup_in_msg();
        }
    }
}

void VDAgent::cleanup_in_msg()
{
    _in_msg_pos = 0;
    delete[] (uint8_t *)_in_msg;
    _in_msg = NULL;
}

void VDAgent::write_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlapped)
{
    VDAgent* a = _singleton;
    VDIChunk* chunk;
    DWORD count;

    ASSERT(!a->_message_queue.empty());
    if (err != 0) {
        vd_printf("vio_serial write completion error %lu", err);
        a->_running = false;
        return;
    }
    MutexLocker lock(a->_message_mutex);
    a->_write_pos += bytes;
    chunk = a->_message_queue.front();
    count = sizeof(VDIChunk) + chunk->hdr.size - a->_write_pos;
    if (count == 0) {
        a->_message_queue.pop();
        a->_write_pos = 0;
        delete chunk;
        if (!a->_message_queue.empty()) {
            chunk = a->_message_queue.front();
            count = sizeof(VDIChunk) + chunk->hdr.size;
        }
    }
    if (count) {
        if (!WriteFileEx(a->_vio_serial, (char*)chunk + a->_write_pos, count, overlapped,
                         write_completion) && GetLastError() != ERROR_IO_PENDING) {
            vd_printf("vio_serial write error %lu", GetLastError());
            a->_running = false;
        }
    }
}

VDIChunk* VDAgent::new_chunk(DWORD bytes)
{
    return (VDIChunk*)(new char[bytes]);
}

void VDAgent::enqueue_chunk(VDIChunk* chunk)
{
    MutexLocker lock(_message_mutex);
    _message_queue.push(chunk);
    if (_message_queue.size() == 1) {
        write_completion(0, 0, &_write_overlapped);
    }
}

LRESULT CALLBACK VDAgent::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    VDAgent* a = _singleton;

    switch (message) {
    case WM_DISPLAYCHANGE:
        vd_printf("Display change");
        // the desktop layout needs to be updated for the mouse
        // position to be scaled correctly
        if (!a->_updating_display_config)
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
    case WM_CLIPBOARDUPDATE:
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
        } else if (wparam == WTS_SESSION_LOCK) {
            a->_session_is_locked = true;
        } else if (wparam == WTS_SESSION_UNLOCK) {
            a->_session_is_locked = false;
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

