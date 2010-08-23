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
#include <lmcons.h>

#define VD_AGENT_LOG_PATH       TEXT("%svdagent.log")
#define VD_AGENT_WINCLASS_NAME  TEXT("VDAGENT")
#define VD_INPUT_INTERVAL_MS    20
#define VD_TIMER_ID             1

class VDAgent {
public:
    static VDAgent* get();
    ~VDAgent();
    bool run();

private:
    VDAgent();
    void input_desktop_message_loop();
    bool handle_mouse_event(VDAgentMouseState* state);
    bool handle_mon_config(VDAgentMonitorsConfig* mon_config, uint32_t port);
    bool handle_control(VDPipeMessage* msg);
    DWORD get_buttons_change(DWORD last_buttons_state, DWORD new_buttons_state,
                             DWORD mask, DWORD down_flag, DWORD up_flag);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static VOID CALLBACK read_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlap);
    static VOID CALLBACK write_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlap);
    static DWORD WINAPI event_thread_proc(LPVOID param);
    uint8_t* write_lock(DWORD bytes = 0);
    void write_unlock(DWORD bytes = 0);
    bool connect_pipe();
    bool send_input();

private:
    static VDAgent* _singleton;
    HWND _hwnd;
    DWORD _buttons_state;
    LONG _mouse_x;
    LONG _mouse_y;
    INPUT _input;
    DWORD _input_time;
    HANDLE _desktop_switch_event;
    bool _pending_input;
    bool _pending_write;
    bool _running;
    DesktopLayout* _desktop_layout;
    VDPipeState _pipe_state;
    mutex_t _write_mutex;
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
    , _buttons_state (0)
    , _mouse_x (0)
    , _mouse_y (0)
    , _input_time (0)
    , _pending_input (false)
    , _pending_write (false)
    , _running (false)
    , _desktop_layout (NULL)
    , _log (NULL)
{
    TCHAR log_path[MAX_PATH];
    TCHAR temp_path[MAX_PATH];

    if (GetTempPath(MAX_PATH, temp_path)) {
        swprintf_s(log_path, MAX_PATH, VD_AGENT_LOG_PATH, temp_path);
        _log = VDLog::get(log_path);
    }
    ZeroMemory(&_input, sizeof(INPUT));
    ZeroMemory(&_pipe_state, sizeof(VDPipeState));
    MUTEX_INIT(_write_mutex);
    _singleton = this;
}

VDAgent::~VDAgent()
{
    delete _log;
}

DWORD WINAPI VDAgent::event_thread_proc(LPVOID param)
{
    HANDLE desktop_event = OpenEvent(SYNCHRONIZE, FALSE, L"WinSta0_DesktopSwitch");
    if (!desktop_event) {
        vd_printf("OpenEvent() failed: %d", GetLastError());
        return 1;
    }
    while (_singleton->_running) {
        DWORD wait_ret = WaitForSingleObject(desktop_event, INFINITE);
        switch (wait_ret) {
        case WAIT_OBJECT_0:
            SetEvent((HANDLE)param);
            break;
        case WAIT_TIMEOUT:
        default:
            vd_printf("WaitForSingleObject(): %u", wait_ret);
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
        vd_printf("ProcessIdToSessionId failed %u", GetLastError());
        return false;
    }
    vd_printf("***Agent started in session %u***", session_id);
    log_version();
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        vd_printf("SetPriorityClass failed %u", GetLastError());
    }
    if (!SetProcessShutdownParameters(0x100, 0)) {
        vd_printf("SetProcessShutdownParameters failed %u", GetLastError());
    }
    _desktop_switch_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!_desktop_switch_event) {
        vd_printf("CreateEvent() failed: %d", GetLastError());
        return false;
    }
    memset(&wcls, 0, sizeof(wcls));
    wcls.lpfnWndProc = &VDAgent::wnd_proc;
    wcls.lpszClassName = VD_AGENT_WINCLASS_NAME;
    if (!RegisterClass(&wcls)) {
        vd_printf("RegisterClass() failed: %d", GetLastError());
        return false;
    }
    _desktop_layout = new DesktopLayout();
    if (_desktop_layout->get_display_count() == 0) {
        vd_printf("No QXL devices!");
    }
    if (!connect_pipe()) {
        CloseHandle(_desktop_switch_event);
        delete _desktop_layout;
        return false;
    }
    _running = true;
    event_thread = CreateThread(NULL, 0, event_thread_proc, _desktop_switch_event, 0,
        &event_thread_id);
    if (!event_thread) {
        vd_printf("CreateThread() failed: %d", GetLastError());
        CloseHandle(_desktop_switch_event);
        CloseHandle(_pipe_state.pipe);
        delete _desktop_layout;
        return false;
    }
    read_completion(0, 0, &_pipe_state.read.overlap);
    while (_running) {
        input_desktop_message_loop();
    }
    vd_printf("Agent stopped");
    CloseHandle(event_thread);
    CloseHandle(_desktop_switch_event);
    CloseHandle(_pipe_state.pipe);
    delete _desktop_layout;
    return true;
}

void VDAgent::input_desktop_message_loop()
{
    bool desktop_switch = false;
    TCHAR desktop_name[MAX_PATH];
    DWORD wait_ret;
    HDESK hdesk;
    MSG msg;

    hdesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hdesk) {
        vd_printf("OpenInputDesktop() failed: %u", GetLastError());
        _running = false;
        return;
    }
    if (!SetThreadDesktop(hdesk)) {
        vd_printf("SetThreadDesktop failed %u", GetLastError());
        _running = false;
        return;
    }
    if (GetUserObjectInformation(hdesk, UOI_NAME, desktop_name, sizeof(desktop_name), NULL)) {
        vd_printf("Desktop: %S", desktop_name);
    } else {
        vd_printf("GetUserObjectInformation failed %u", GetLastError());
    }
    _hwnd = CreateWindow(VD_AGENT_WINCLASS_NAME, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!_hwnd) {
        vd_printf("CreateWindow() failed: %u", GetLastError());
        _running = false;
        return;
    }
    while (_running && !desktop_switch) {
        wait_ret = MsgWaitForMultipleObjectsEx(1, &_desktop_switch_event, INFINITE, QS_ALLINPUT,
                                               MWMO_ALERTABLE);
        switch (wait_ret) {
        case WAIT_OBJECT_0:
            vd_printf("WinSta0_DesktopSwitch");
            desktop_switch = true;
            break;
        case WAIT_OBJECT_0 + 1:
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            break;
        case WAIT_IO_COMPLETION:
            break;
        case WAIT_TIMEOUT:
        default:
            vd_printf("MsgWaitForMultipleObjectsEx(): %u", wait_ret);
        }
    }
    if (_pending_input) {
        KillTimer(_hwnd, VD_TIMER_ID);
        _pending_input = false;
    }
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
            vd_printf("KillTimer failed: %d", GetLastError());
            _running = false;
            _desktop_layout->unlock();
            return false;
        }
    }
    if (!SendInput(1, &_input, sizeof(INPUT)) && GetLastError() != ERROR_ACCESS_DENIED) {
        vd_printf("SendInput failed: %d", GetLastError());
        ret = _running = false;
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
            vd_printf("SetTimer failed: %d", GetLastError());
            _running = false;
            ret = false;
        }
    }
    _desktop_layout->unlock();
    return ret;
}

bool VDAgent::handle_mon_config(VDAgentMonitorsConfig* mon_config, uint32_t port)
{
    VDPipeMessage* reply_pipe_msg;
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
    reply_pipe_msg = (VDPipeMessage*)write_lock(msg_size);
    if (!reply_pipe_msg) {
        return false;
    }
    reply_pipe_msg->type = VD_AGENT_COMMAND;
    reply_pipe_msg->opaque = port;
    reply_msg = (VDAgentMessage*)reply_pipe_msg->data;
    reply_msg->protocol = VD_AGENT_PROTOCOL;
    reply_msg->type = VD_AGENT_REPLY;
    reply_msg->opaque = 0;
    reply_msg->size = sizeof(VDAgentReply);
    reply = (VDAgentReply*)reply_msg->data;
    reply->type = VD_AGENT_MONITORS_CONFIG;
    reply->error = display_count ? VD_AGENT_SUCCESS : VD_AGENT_ERROR;
    write_unlock(msg_size);
    if (!_pending_write) {
        write_completion(0, 0, &_pipe_state.write.overlap);
    }
    return true;
}

bool VDAgent::handle_control(VDPipeMessage* msg)
{
    switch (msg->type) {
    case VD_AGENT_RESET: {
        vd_printf("Agent reset");
        VDPipeMessage* ack = (VDPipeMessage*)write_lock(sizeof(VDPipeMessage));
        if (!ack) {
            return false;
        }
        ack->type = VD_AGENT_RESET_ACK;
        ack->opaque = msg->opaque;
        write_unlock(sizeof(VDPipeMessage));
        if (!_pending_write) {
            write_completion(0, 0, &_pipe_state.write.overlap);
        }
        break;
    }
    case VD_AGENT_QUIT:
        vd_printf("Agent quit");
        _running = false;
        break;
    default:
        vd_printf("Unsupported control %u", msg->type);
        return false;
    }
    return true;
}

bool VDAgent::connect_pipe()
{
    VDAgent* a = _singleton;
    HANDLE pipe;

    ZeroMemory(&a->_pipe_state, sizeof(VDPipeState));
    if (!WaitNamedPipe(VD_SERVICE_PIPE_NAME, NMPWAIT_USE_DEFAULT_WAIT)) {
        vd_printf("WaitNamedPipe() failed: %d", GetLastError());
        return false;
    }
    //assuming vdservice created the named pipe before creating this vdagent process
    pipe = CreateFile(VD_SERVICE_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                      0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        vd_printf("CreateFile() failed: %d", GetLastError());
        return false;
    }
    DWORD pipe_mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(pipe, &pipe_mode, NULL, NULL)) {
        vd_printf("SetNamedPipeHandleState() failed: %d", GetLastError());
        CloseHandle(pipe);
        return false;
    }
    a->_pipe_state.pipe = pipe;
    vd_printf("Connected to service pipe");
    return true;
}

VOID CALLBACK VDAgent::read_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlap)
{
    VDAgent* a = _singleton;
    VDPipeState* ps = &a->_pipe_state;
    DWORD len;

    if (!a->_running) {
        return;
    }
    if (err) {
        vd_printf("error %u", err);
        a->_running = false;
        return;
    }
    ps->read.end += bytes;
    while (a->_running && (len = ps->read.end - ps->read.start) >= sizeof(VDPipeMessage)) {
        VDPipeMessage* pipe_msg = (VDPipeMessage*)&ps->read.data[ps->read.start];

        if (pipe_msg->type != VD_AGENT_COMMAND) {
            a->handle_control(pipe_msg);
            ps->read.start += sizeof(VDPipeMessage);
            continue;
        }
        if (len < VD_MESSAGE_HEADER_SIZE) {
            break;
        }
        VDAgentMessage* msg = (VDAgentMessage*)pipe_msg->data;
        if (len < VD_MESSAGE_HEADER_SIZE + msg->size) {
            break;
        }
        if (msg->protocol != VD_AGENT_PROTOCOL) {
            vd_printf("Invalid protocol %d", msg->protocol);
            a->_running = false;
            break;
        }
        switch (msg->type) {
        case VD_AGENT_MOUSE_STATE:
            if (!a->handle_mouse_event((VDAgentMouseState*)msg->data)) {
                vd_printf("handle_mouse_event failed: %d", GetLastError());
                a->_running = false;
            }
            break;
        case VD_AGENT_MONITORS_CONFIG:
            if (!a->handle_mon_config((VDAgentMonitorsConfig*)msg->data, pipe_msg->opaque)) {
                vd_printf("handle_mon_config failed: %d", GetLastError());
                a->_running = false;
            }
            break;
        default:
            vd_printf("Unsupported message type %d size %d", msg->type, msg->size);
        }
        ps->read.start += (VD_MESSAGE_HEADER_SIZE + msg->size);
        if (ps->read.start == ps->read.end) {
            ps->read.start = ps->read.end = 0;
        }
    }
    if (a->_running && ps->read.end < sizeof(ps->read.data) &&
        !ReadFileEx(ps->pipe, ps->read.data + ps->read.end, sizeof(ps->read.data) - ps->read.end,
                    overlap, read_completion)) {
        vd_printf("ReadFileEx() failed: %u", GetLastError());
        a->_running = false;
    }
}

VOID CALLBACK VDAgent::write_completion(DWORD err, DWORD bytes, LPOVERLAPPED overlap)
{
    VDAgent* a = _singleton;
    VDPipeState* ps = &a->_pipe_state;

    a->_pending_write = false;
    if (!a->_running) {
        return;
    }
    if (err) {
        vd_printf("error %u", err);
        a->_running = false;
        return;
    }
    if (!a->write_lock()) {
        a->_running = false;
        return;
    }
    ps->write.start += bytes;
    if (ps->write.start == ps->write.end) {
        ps->write.start = ps->write.end = 0;
    } else if (WriteFileEx(ps->pipe, ps->write.data + ps->write.start,
                           ps->write.end - ps->write.start, overlap, write_completion)) {
        a->_pending_write = true;
    } else {
        vd_printf("WriteFileEx() failed: %u", GetLastError());
        a->_running = false;
    }
    a->write_unlock();
}

uint8_t* VDAgent::write_lock(DWORD bytes)
{
    if (_pipe_state.write.end + bytes <= sizeof(_pipe_state.write.data)) {
        MUTEX_LOCK(_write_mutex);
        return &_pipe_state.write.data[_pipe_state.write.end];
    } else {
        vd_printf("write buffer is full");
        return NULL;
    }
}

void VDAgent::write_unlock(DWORD bytes)
{
    _pipe_state.write.end += bytes;
    MUTEX_UNLOCK(_write_mutex);
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
    default:
        return DefWindowProc(hwnd, message, wparam, lparam);
    }
    return 0;
}

int APIENTRY _tWinMain(HINSTANCE instance, HINSTANCE prev_instance, LPTSTR cmd_line, int cmd_show)
{
    VDAgent* vdagent = VDAgent::get();
    vdagent->run();
    delete vdagent;
    return 0;
}
