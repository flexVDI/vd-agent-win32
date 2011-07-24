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

#include <windows.h>
#include <winternl.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <queue>
#include "vdcommon.h"
#include "virtio_vdi_port.h"
#include "pci_vdi_port.h"

//#define DEBUG_VDSERVICE

#define VD_SERVICE_DISPLAY_NAME TEXT("RHEV Spice Agent")
#define VD_SERVICE_NAME         TEXT("vdservice")
#define VD_SERVICE_DESCRIPTION  TEXT("Enables Spice event injection and display configuration.")
#define VD_SERVICE_LOG_PATH     TEXT("%svdservice.log")
#define VD_SERVICE_LOAD_ORDER_GROUP TEXT("Pointer Port")
#define VD_AGENT_PATH           TEXT("%s\\vdagent.exe")
#define VD_AGENT_TIMEOUT        3000
#define VD_AGENT_MAX_RESTARTS   10
#define VD_AGENT_RESTART_INTERVAL 3000
#define VD_AGENT_RESTART_COUNT_RESET_INTERVAL 60000
#define WINLOGON_FILENAME       TEXT("winlogon.exe")
#define CREATE_PROC_MAX_RETRIES 10
#define CREATE_PROC_INTERVAL_MS 500

// This enum simplifies WaitForMultipleEvents for static
// events, that is handles that are guranteed non NULL.
// It doesn't include:
// VirtioVDIPort Handles - these are filled by an interface because
//  of variable handle number.
// VDAgent handle - this can be 1 or 0 (NULL or not), so it is also added at
//  the end of VDService::_events
enum {
    VD_EVENT_PIPE_READ = 0,
    VD_EVENT_PIPE_WRITE,
    VD_EVENT_CONTROL,
    VD_STATIC_EVENTS_COUNT // Must be last
};

enum {
    VD_CONTROL_IDLE = 0,
    VD_CONTROL_STOP,
    VD_CONTROL_LOGON,
    VD_CONTROL_RESTART_AGENT,
};

typedef std::queue<int> VDControlQueue;

class VDService {
public:
    static VDService* get();
    ~VDService();
    bool run();
    bool install();
    bool uninstall();

private:
    VDService();
    bool execute();
    void stop();
    static DWORD WINAPI control_handler(DWORD control, DWORD event_type,
                                        LPVOID event_data, LPVOID context);
    static VOID WINAPI main(DWORD argc, TCHAR * argv[]);
    bool init_vdi_port();
    void set_control_event(int control_command);
    void handle_control_event();
    void pipe_write_completion();
    void write_agent_control(uint32_t type, uint32_t opaque);
    void read_pipe();
    void handle_pipe_data(DWORD bytes);
    void handle_port_data();
    bool handle_agent_control(VDPipeMessage* msg);
    bool restart_agent(bool normal_restart);
    bool launch_agent();
    bool kill_agent();
    unsigned fill_agent_event() {
        ASSERT(_events);
        if (_agent_proc_info.hProcess) {
            _events[_events_count - 1] = _agent_proc_info.hProcess;
            return _events_count;
        } else {
            return _events_count - 1;
        }
    }
private:
    static VDService* _singleton;
    SERVICE_STATUS _status;
    SERVICE_STATUS_HANDLE _status_handle;
    PROCESS_INFORMATION _agent_proc_info;
    HANDLE _control_event;
    HANDLE* _events;
    TCHAR _agent_path[MAX_PATH];
    VDIPort* _vdi_port;
    VDPipeState _pipe_state;
    VDControlQueue _control_queue;
    mutex_t _control_mutex;
    mutex_t _agent_mutex;
    uint32_t _connection_id;
    DWORD _session_id;
    DWORD _chunk_port;
    DWORD _chunk_size;
    DWORD _last_agent_restart_time;
    int _agent_restarts;
    int _system_version;
    bool _pipe_connected;
    bool _pending_reset;
    bool _pending_write;
    bool _pending_read;
    bool _agent_alive;
    bool _running;
    VDLog* _log;
    unsigned _events_count;
    unsigned _events_vdi_port_base;
};

VDService* VDService::_singleton = NULL;

VDService* VDService::get()
{
    if (!_singleton) {
        _singleton = new VDService();
    }
    return (VDService*)_singleton;
}

enum SystemVersion {
    SYS_VER_UNSUPPORTED,
    SYS_VER_WIN_XP_CLASS, // also Server 2003/R2
    SYS_VER_WIN_7_CLASS,  // also Server 2008/R2 & Vista
};

int supported_system_version()
{
    OSVERSIONINFOEX osvi;

    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    if (!GetVersionEx((OSVERSIONINFO*)&osvi)) {
        vd_printf("GetVersionEx() failed: %u", GetLastError());
        return 0;
    }
    if (osvi.dwMajorVersion == 5 && (osvi.dwMinorVersion == 1 || osvi.dwMinorVersion == 2)) {
        return SYS_VER_WIN_XP_CLASS;
    } else if (osvi.dwMajorVersion == 6 && (osvi.dwMinorVersion == 0 || osvi.dwMinorVersion == 1)) {
        return SYS_VER_WIN_7_CLASS;
    }
    return 0;
}

VDService::VDService()
    : _status_handle (0)
    , _events (NULL)
    , _vdi_port (NULL)
    , _connection_id (0)
    , _session_id (0)
    , _chunk_port (0)
    , _chunk_size (0)
    , _last_agent_restart_time (0)
    , _agent_restarts (0)
    , _pipe_connected (false)
    , _pending_reset (false)
    , _pending_write (false)
    , _pending_read (false)
    , _agent_alive (false)
    , _running (false)
    , _log (NULL)
    , _events_count(0)
    , _events_vdi_port_base(0)
{
    ZeroMemory(&_agent_proc_info, sizeof(_agent_proc_info));
    ZeroMemory(&_pipe_state, sizeof(_pipe_state));
    _system_version = supported_system_version();
    _control_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    _pipe_state.write.overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    _pipe_state.read.overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    _agent_path[0] = wchar_t('\0');
    MUTEX_INIT(_agent_mutex);
    MUTEX_INIT(_control_mutex);
    _singleton = this;
}

VDService::~VDService()
{
    CloseHandle(_pipe_state.read.overlap.hEvent);
    CloseHandle(_pipe_state.write.overlap.hEvent);
    CloseHandle(_control_event);
    delete _events;
    delete _log;
}

bool VDService::run()
{
#ifndef DEBUG_VDSERVICE
    SERVICE_TABLE_ENTRY service_table[] = {{VD_SERVICE_NAME, main}, {0, 0}};
    return !!StartServiceCtrlDispatcher(service_table);
#else
    main(0, NULL);
    return true;
#endif
}

bool VDService::install()
{
    bool ret = false;

    SC_HANDLE service_control_manager = OpenSCManager(0, 0, SC_MANAGER_CREATE_SERVICE);
    if (!service_control_manager) {
        printf("OpenSCManager failed\n");
        return false;
    }
    TCHAR path[_MAX_PATH + 1];
    if (!GetModuleFileName(0, path, sizeof(path) / sizeof(path[0]))) {
        printf("GetModuleFileName failed\n");
        CloseServiceHandle(service_control_manager);
        return false;
    }
    SC_HANDLE service = CreateService(service_control_manager, VD_SERVICE_NAME,
                                      VD_SERVICE_DISPLAY_NAME, SERVICE_ALL_ACCESS,
                                      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                      SERVICE_ERROR_IGNORE, path, VD_SERVICE_LOAD_ORDER_GROUP,
                                      0, 0, 0, 0);
    if (service) {
        SERVICE_DESCRIPTION descr;
        descr.lpDescription = VD_SERVICE_DESCRIPTION;
        if (!ChangeServiceConfig2(service, SERVICE_CONFIG_DESCRIPTION, &descr)) {
            printf("ChangeServiceConfig2 failed\n");
        }
        CloseServiceHandle(service);
        printf("Service installed successfully\n");
        ret = true;
    } else if (GetLastError() == ERROR_SERVICE_EXISTS) {
        printf("Service already exists\n");
        ret = true;
    } else {
        printf("Service not installed successfully, error %d\n", GetLastError());
    }
    CloseServiceHandle(service_control_manager);
    return ret;
}

bool VDService::uninstall()
{
    bool ret = false;

    SC_HANDLE service_control_manager = OpenSCManager(0, 0, SC_MANAGER_CONNECT);
    if (!service_control_manager) {
        printf("OpenSCManager failed\n");
        return false;
    }
    SC_HANDLE service = OpenService(service_control_manager, VD_SERVICE_NAME,
                                    SERVICE_QUERY_STATUS | DELETE);
    if (!service) {
        printf("OpenService failed\n");
        CloseServiceHandle(service_control_manager);
        return false;
    }
    SERVICE_STATUS status;
    if (!QueryServiceStatus(service, &status)) {
        printf("QueryServiceStatus failed\n");
    } else if (status.dwCurrentState != SERVICE_STOPPED) {
        printf("Service is still running\n");
    } else if (DeleteService(service)) {
        printf("Service removed successfully\n");
        ret = true;
    } else {
        switch (GetLastError()) {
        case ERROR_ACCESS_DENIED:
            printf("Access denied while trying to remove service\n");
            break;
        case ERROR_INVALID_HANDLE:
            printf("Handle invalid while trying to remove service\n");
            break;
        case ERROR_SERVICE_MARKED_FOR_DELETE:
            printf("Service already marked for deletion\n");
            break;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(service_control_manager);
    return ret;
}

const char* session_events[] = {
    "INVALID", "CONNECT", "DISCONNECT", "REMOTE_CONNECT", "REMOTE_DISCONNECT", "LOGON", "LOGOFF",
    "LOCK", "UNLOCK", "REMOTE_CONTROL"
};

void VDService::set_control_event(int control_command)
{
    MUTEX_LOCK(_control_mutex);
    _control_queue.push(control_command);
    if (_control_event && !SetEvent(_control_event)) {
        vd_printf("SetEvent() failed: %u", GetLastError());
    }
    MUTEX_UNLOCK(_control_mutex);
}

void VDService::handle_control_event()
{
    MUTEX_LOCK(_control_mutex);
    while (_control_queue.size()) {
        int control_command = _control_queue.front();
        _control_queue.pop();
        vd_printf("Control command %d", control_command);
        switch (control_command) {
        case VD_CONTROL_STOP:
            _running = false;
            break;
        case VD_CONTROL_LOGON:
            write_agent_control(VD_AGENT_SESSION_LOGON, 0);
            break;
        case VD_CONTROL_RESTART_AGENT:
            _running = restart_agent(true);
            break;
        default:
            vd_printf("Unsupported control command %u", control_command);
        }
    }
    MUTEX_UNLOCK(_control_mutex);

}

DWORD WINAPI VDService::control_handler(DWORD control, DWORD event_type, LPVOID event_data,
                                        LPVOID context)
{
    VDService* s = _singleton;
    DWORD ret = NO_ERROR;

    ASSERT(s);
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        vd_printf("Stop service");
        s->_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(s->_status_handle, &s->_status);
        s->stop();
        break;
    case SERVICE_CONTROL_INTERROGATE:
        vd_printf("Interrogate service");
        SetServiceStatus(s->_status_handle, &s->_status);
        break;
    case SERVICE_CONTROL_SESSIONCHANGE: {
        DWORD session_id = ((WTSSESSION_NOTIFICATION*)event_data)->dwSessionId;
        vd_printf("Session %u %s", session_id, session_events[event_type]);
        SetServiceStatus(s->_status_handle, &s->_status);
        if (s->_system_version != SYS_VER_UNSUPPORTED) {
            if (event_type == WTS_CONSOLE_CONNECT) {
                s->_session_id = session_id;
                s->set_control_event(VD_CONTROL_RESTART_AGENT);
            } else if (event_type == WTS_SESSION_LOGON) {
                s->set_control_event(VD_CONTROL_LOGON);
            }
        }
        break;
    }
    default:
        vd_printf("Unsupported control %u", control);
        ret = ERROR_CALL_NOT_IMPLEMENTED;
    }
    return ret;
}

#define VDSERVICE_ACCEPTED_CONTROLS \
    (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE)

VOID WINAPI VDService::main(DWORD argc, TCHAR* argv[])
{
    VDService* s = _singleton;
    SERVICE_STATUS* status;
    TCHAR log_path[MAX_PATH];
    TCHAR full_path[MAX_PATH];
    TCHAR temp_path[MAX_PATH];
    TCHAR* slash;

    ASSERT(s);
    if (GetModuleFileName(NULL, full_path, MAX_PATH) && (slash = wcsrchr(full_path, TCHAR('\\'))) &&
        GetTempPath(MAX_PATH, temp_path)) {
        *slash = TCHAR('\0');
        swprintf_s(s->_agent_path, MAX_PATH, VD_AGENT_PATH, full_path);
        swprintf_s(log_path, MAX_PATH, VD_SERVICE_LOG_PATH, temp_path);
        s->_log = VDLog::get(log_path);
    }
    vd_printf("***Service started***");
    log_version();
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        vd_printf("SetPriorityClass failed %u", GetLastError());
    }
    status = &s->_status;
    status->dwServiceType = SERVICE_WIN32;
    status->dwCurrentState = SERVICE_STOPPED;
    status->dwControlsAccepted = 0;
    status->dwWin32ExitCode = NO_ERROR;
    status->dwServiceSpecificExitCode = NO_ERROR;
    status->dwCheckPoint = 0;
    status->dwWaitHint = 0;
    s->_status_handle = RegisterServiceCtrlHandlerEx(VD_SERVICE_NAME, &VDService::control_handler,
                                                     NULL);
    if (!s->_status_handle) {
        printf("RegisterServiceCtrlHandler failed\n");
#ifndef DEBUG_VDSERVICE
        return;
#endif // DEBUG_VDSERVICE
    }

    // service is starting
    status->dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(s->_status_handle, status);

    // service running
    status->dwControlsAccepted |= VDSERVICE_ACCEPTED_CONTROLS;
    status->dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(s->_status_handle, status);

    s->_running = true;
    s->execute();

    // service was stopped
    status->dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(s->_status_handle, status);

    // service is stopped
    status->dwControlsAccepted &= ~VDSERVICE_ACCEPTED_CONTROLS;
    status->dwCurrentState = SERVICE_STOPPED;
#ifndef DEBUG_VDSERVICE
    SetServiceStatus(s->_status_handle, status);
#endif //DEBUG_VDSERVICE
}

VDIPort *create_virtio_vdi_port()
{
    return new VirtioVDIPort();
}

VDIPort *create_pci_vdi_port()
{
    return new PCIVDIPort();
}

bool VDService::init_vdi_port()
{
    VDIPort* (*creators[])(void) = { create_virtio_vdi_port, create_pci_vdi_port };

    for (int i = 0 ; i < sizeof(creators)/sizeof(creators[0]); ++i) {
        _vdi_port = creators[i]();
        if (_vdi_port->init()) {
            return true;
        }
        delete _vdi_port;
    }
    return false;
}

bool VDService::execute()
{
    SECURITY_ATTRIBUTES sec_attr;
    SECURITY_DESCRIPTOR* sec_desr;
    HANDLE pipe;

    sec_desr = (SECURITY_DESCRIPTOR*)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(sec_desr, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(sec_desr, TRUE, (PACL)NULL, FALSE);
    sec_attr.nLength = sizeof(sec_attr);
    sec_attr.bInheritHandle = TRUE;
    sec_attr.lpSecurityDescriptor = sec_desr;
    pipe = CreateNamedPipe(VD_SERVICE_PIPE_NAME, PIPE_ACCESS_DUPLEX |
                           FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                           PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                           PIPE_UNLIMITED_INSTANCES, BUF_SIZE, BUF_SIZE,
                           VD_AGENT_TIMEOUT, &sec_attr);
    LocalFree(sec_desr);
    if (pipe == INVALID_HANDLE_VALUE) {
        vd_printf("CreatePipe() failed: %u", GetLastError());
        return false;
    }
    _pipe_state.pipe = pipe;
    _session_id = WTSGetActiveConsoleSessionId();
    if (_session_id == 0xFFFFFFFF) {
        vd_printf("WTSGetActiveConsoleSessionId() failed");
        CloseHandle(pipe);
        return false;
    }
    if (!launch_agent()) {
        CloseHandle(pipe);
        return false;
    }

    if (!init_vdi_port()) {
        vd_printf("Failed to create VDIPort instance");
        CloseHandle(pipe);
        return false;
    }
    vd_printf("created %s", _vdi_port->name());
    _events_count = VD_STATIC_EVENTS_COUNT + _vdi_port->get_num_events() + 1 /*for agent*/;
    _events = new HANDLE[_events_count];
    _events_vdi_port_base = VD_STATIC_EVENTS_COUNT;
    ZeroMemory(_events, _events_count);
    vd_printf("Connected to server");
    _events[VD_EVENT_PIPE_READ] = _pipe_state.read.overlap.hEvent;
    _events[VD_EVENT_PIPE_WRITE] = _pipe_state.write.overlap.hEvent;
    _events[VD_EVENT_CONTROL] = _control_event;
    _agent_proc_info.hProcess;
    _vdi_port->fill_events(&_events[_events_vdi_port_base]);
    _chunk_size = _chunk_port = 0;
    read_pipe();
    while (_running) {
        int cont_read = _vdi_port->read();
        int cont_write = _vdi_port->write();
        bool cont = false;

        if (cont_read >= 0 && cont_write >= 0) {
            cont = cont_read || cont_write;
        } else if (cont_read == VDI_PORT_ERROR || cont_write == VDI_PORT_ERROR) {
            vd_printf("VDI Port error, read %d write %d", cont_read, cont_write);
            _running = false;
        } else if (cont_read == VDI_PORT_RESET || cont_write == VDI_PORT_RESET) {
            vd_printf("VDI Port reset, read %d write %d", cont_read, cont_write);
            _chunk_size = _chunk_port = 0;
            write_agent_control(VD_AGENT_RESET, ++_connection_id);
            _pending_reset = true;
        }
        if (cont) {
            handle_port_data();
        }
        if (cont_write) {
            handle_pipe_data(0);
        }
        if (_running && (!cont || _pending_read || _pending_write)) {
            unsigned actual_events = fill_agent_event();
            DWORD wait_ret = WaitForMultipleObjects(actual_events, _events, FALSE,
                                                              cont ? 0 : INFINITE);
            switch (wait_ret) {
            case WAIT_OBJECT_0 + VD_EVENT_PIPE_READ: {
                DWORD bytes = 0;
                if (_pipe_connected && _pending_read) {
                    _pending_read = false;
                    if (GetOverlappedResult(_pipe_state.pipe, &_pipe_state.read.overlap,
                                            &bytes, FALSE) || GetLastError() == ERROR_MORE_DATA) {
                        handle_pipe_data(bytes);
                        read_pipe();
                    } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
                        vd_printf("GetOverlappedResult failed %u", GetLastError());
                        _pipe_connected = false;
                        DisconnectNamedPipe(_pipe_state.pipe);
                    }
                }
                break;
            }
            case WAIT_OBJECT_0 + VD_EVENT_PIPE_WRITE:
                pipe_write_completion();
                break;
            case WAIT_OBJECT_0 + VD_EVENT_CONTROL:
                handle_control_event();
                break;
            case WAIT_TIMEOUT:
                break;
            default:
                if (wait_ret == WAIT_OBJECT_0 + _events_count - 1) {
                    vd_printf("Agent killed");
                    if (_system_version == SYS_VER_WIN_XP_CLASS) {
                        restart_agent(false);
                    } else if (_system_version == SYS_VER_WIN_7_CLASS) {
                        kill_agent();
                    }
                } else {
                    if (wait_ret >= WAIT_OBJECT_0 + _events_vdi_port_base &&
                        wait_ret < WAIT_OBJECT_0 +
                                   _events_vdi_port_base + _vdi_port->get_num_events()) {
                        _vdi_port->handle_event(wait_ret - VD_STATIC_EVENTS_COUNT - WAIT_OBJECT_0);
                    } else {
                        vd_printf("WaitForMultipleObjects failed %u", GetLastError());
                    }
                }
            }
        }
    }
    delete _vdi_port;
    CloseHandle(pipe);
    return true;
}

DWORD64 marshall_string(LPCWSTR str, DWORD max_size, LPBYTE* next_buf, DWORD* used_bytes)
{
    DWORD offset = *used_bytes;

    if (!str) {
        return 0;
    }
    DWORD len = (DWORD)(wcslen(str) + 1) * sizeof(WCHAR);
    if (*used_bytes + len > max_size) {
        return 0;
    }
    memmove(*next_buf, str, len);
    *used_bytes += len;
    *next_buf += len;
    return offset;
}

typedef struct CreateProcessParams {
    DWORD size;
    DWORD process_id;
    BOOL use_default_token;
    HANDLE token;
    LPWSTR application_name;
    LPWSTR command_line;
    SECURITY_ATTRIBUTES process_attributes;
    SECURITY_ATTRIBUTES thread_attributes;
    BOOL inherit_handles;
    DWORD creation_flags;
    LPVOID environment;
    LPWSTR current_directory;
    STARTUPINFOW startup_info;
    PROCESS_INFORMATION process_information;
    BYTE data[0x2000];
} CreateProcessParams;

typedef struct CreateProcessRet {
    DWORD size;
    BOOL ret_value;
    DWORD last_error;
    PROCESS_INFORMATION process_information;
} CreateProcessRet;

BOOL create_session_process_as_user(IN DWORD session_id, IN BOOL use_default_token, IN HANDLE token,
                                    IN LPCWSTR application_name, IN LPWSTR command_line,
                                    IN LPSECURITY_ATTRIBUTES process_attributes,
                                    IN LPSECURITY_ATTRIBUTES thread_attributes,
                                    IN BOOL inherit_handles, IN DWORD creation_flags,
                                    IN LPVOID environment, IN LPCWSTR current_directory,
                                    IN LPSTARTUPINFOW startup_info,
                                    OUT LPPROCESS_INFORMATION process_information)
{
    WCHAR win_sta_path[MAX_PATH];
    HINSTANCE win_sta_handle;
    WCHAR pipe_name[MAX_PATH] = L"";
    DWORD pipe_name_len;
    BOOL got_pipe_name = FALSE;
    HANDLE named_pipe;
    CreateProcessRet proc_ret;
    CreateProcessParams proc_params;
    LPBYTE buffer = (LPBYTE)proc_params.data;
    DWORD max_size = sizeof(proc_params);
    DWORD bytes_used = offsetof(CreateProcessParams, data);
    DWORD bytes_written;
    DWORD bytes_read;
    DWORD env_len = 0;
    BOOL ret = FALSE;

    GetSystemDirectoryW(win_sta_path, MAX_PATH);
    lstrcatW(win_sta_path, L"\\winsta.dll");
    win_sta_handle = LoadLibrary(win_sta_path);
    if (win_sta_handle) {
        PWINSTATIONQUERYINFORMATIONW win_sta_query_func =
            (PWINSTATIONQUERYINFORMATIONW)GetProcAddress(win_sta_handle,
                                                         "WinStationQueryInformationW");
        if (win_sta_query_func) {
            got_pipe_name = win_sta_query_func(0, session_id, (WINSTATIONINFOCLASS)0x21,
                                               pipe_name, sizeof(pipe_name), &pipe_name_len);
        }
        FreeLibrary(win_sta_handle);
    }
    if (!got_pipe_name || pipe_name[0] == '\0') {
        swprintf_s(pipe_name, MAX_PATH, L"\\\\.\\Pipe\\TerminalServer\\SystemExecSrvr\\%d",
                   session_id);
    }

    do {
        named_pipe = CreateFile(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                0, 0);
        if (named_pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                if (!WaitNamedPipe(pipe_name, 3000)) {
                    return FALSE;
                }
            } else {
                return FALSE;
            }
        }
    } while (named_pipe == INVALID_HANDLE_VALUE);

    memset(&proc_params, 0, sizeof(proc_params));
    proc_params.process_id = GetCurrentProcessId();
    proc_params.use_default_token = use_default_token;
    proc_params.token = token;
    proc_params.application_name = (LPWSTR)marshall_string(application_name, max_size, &buffer,
                                                           &bytes_used);
    proc_params.command_line = (LPWSTR)marshall_string(command_line, max_size, &buffer,
                                                       &bytes_used);
    if (process_attributes) {
        proc_params.process_attributes = *process_attributes;
    }
    if (thread_attributes) {
        proc_params.thread_attributes = *thread_attributes;
    }
    proc_params.inherit_handles = inherit_handles;
    proc_params.creation_flags = creation_flags;
    proc_params.current_directory = (LPWSTR)marshall_string(current_directory, max_size,
                                                            &buffer, &bytes_used);
    if (startup_info) {
        proc_params.startup_info = *startup_info;
        proc_params.startup_info.lpDesktop = (LPWSTR)marshall_string(startup_info->lpDesktop,
                                                                     max_size, &buffer,
                                                                     &bytes_used);
        proc_params.startup_info.lpTitle = (LPWSTR)marshall_string(startup_info->lpTitle,
                                                                   max_size, &buffer, &bytes_used);
    }
    if (environment) {
        if (creation_flags & CREATE_UNICODE_ENVIRONMENT) {
            while ((env_len + bytes_used <= max_size)) {
                if (((LPWSTR)environment)[env_len / 2] == '\0' &&
                        ((LPWSTR)environment)[env_len / 2 + 1] == '\0') {
                    env_len += 2 * sizeof(WCHAR);
                    break;
                }
                env_len += sizeof(WCHAR);
            }
        } else {
            while (env_len + bytes_used <= max_size) {
                if (((LPSTR)environment)[env_len] == '\0' &&
                        ((LPSTR)environment)[env_len + 1] == '\0') {
                    env_len += 2;
                    break;
                }
                env_len++;
            }
        }
        if (env_len + bytes_used <= max_size) {
            memmove(buffer, environment, env_len);
            proc_params.environment = (LPVOID)(UINT64)bytes_used;
            buffer += env_len;
            bytes_used += env_len;
        } else {
            proc_params.environment = NULL;
        }
    } else {
        proc_params.environment = NULL;
    }
    proc_params.size = bytes_used;

    if (WriteFile(named_pipe, &proc_params, proc_params.size, &bytes_written, NULL) &&
        ReadFile(named_pipe, &proc_ret, sizeof(proc_ret), &bytes_read, NULL)) {
        ret = proc_ret.ret_value;
        if (ret) {
            *process_information = proc_ret.process_information;
        } else {
            SetLastError(proc_ret.last_error);
        }
    } else {
        ret = FALSE;
    }
    CloseHandle(named_pipe);
    return ret;
}

BOOL create_process_as_user(IN DWORD session_id, IN LPCWSTR application_name,
                            IN LPWSTR command_line, IN LPSECURITY_ATTRIBUTES process_attributes,
                            IN LPSECURITY_ATTRIBUTES thread_attributes, IN BOOL inherit_handles,
                            IN DWORD creation_flags, IN LPVOID environment,
                            IN LPCWSTR current_directory, IN LPSTARTUPINFOW startup_info,
                            OUT LPPROCESS_INFORMATION process_information)
{
    PROCESSENTRY32 proc_entry;
    DWORD winlogon_pid = 0;
    HANDLE winlogon_proc;
    HANDLE token = NULL;
    HANDLE token_dup;
    BOOL ret = FALSE;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        vd_printf("CreateToolhelp32Snapshot() failed %u", GetLastError());
        return false;
    }
    ZeroMemory(&proc_entry, sizeof(proc_entry));
    proc_entry.dwSize = sizeof(PROCESSENTRY32);
    if (!Process32First(snap, &proc_entry)) {
        vd_printf("Process32First() failed %u", GetLastError());
        CloseHandle(snap);
        return false;
    }
    do {
        if (_tcsicmp(proc_entry.szExeFile, WINLOGON_FILENAME) == 0) {
            DWORD winlogon_session_id = 0;
            if (ProcessIdToSessionId(proc_entry.th32ProcessID, &winlogon_session_id) &&
                                                      winlogon_session_id == session_id) {
                winlogon_pid = proc_entry.th32ProcessID;
                break;
            }
        }
    } while (Process32Next(snap, &proc_entry));
    CloseHandle(snap);
    if (winlogon_pid == 0) {
        vd_printf("Winlogon not found");
        return false;
    }
    winlogon_proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogon_pid);
    if (!winlogon_proc) {
        vd_printf("OpenProcess() failed %u", GetLastError());
        return false;
    }
    ret = OpenProcessToken(winlogon_proc, TOKEN_DUPLICATE, &token);
    CloseHandle(winlogon_proc);
    if (!ret) {
        vd_printf("OpenProcessToken() failed %u", GetLastError());
        return false;
    }
    ret = DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary,
                           &token_dup);
    CloseHandle(token);
    if (!ret) {
        vd_printf("DuplicateTokenEx() failed %u", GetLastError());
        return false;
    }
    ret = CreateProcessAsUser(token_dup, application_name, command_line, process_attributes,
                              thread_attributes, inherit_handles, creation_flags, environment,
                              current_directory, startup_info, process_information);
    CloseHandle(token_dup);
    return ret;
}

bool VDService::launch_agent()
{
    STARTUPINFO startup_info;
    OVERLAPPED overlap;
    BOOL ret = FALSE;

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.lpDesktop = TEXT("Winsta0\\winlogon");
    ZeroMemory(&_agent_proc_info, sizeof(_agent_proc_info));
    if (_system_version == SYS_VER_WIN_XP_CLASS) {
        if (_session_id == 0) {
            ret = CreateProcess(_agent_path, _agent_path, NULL, NULL, FALSE, 0, NULL, NULL,
                                &startup_info, &_agent_proc_info);
        } else {
            for (int i = 0; i < CREATE_PROC_MAX_RETRIES; i++) {
                ret = create_session_process_as_user(_session_id, TRUE, NULL, NULL, _agent_path,
                                                     NULL, NULL, FALSE, 0, NULL, NULL,
                                                     &startup_info, &_agent_proc_info);
                if (ret) {
                    vd_printf("create_session_process_as_user #%d", i);
                    break;
                }
                Sleep(CREATE_PROC_INTERVAL_MS);
            }
        }
    } else if (_system_version == SYS_VER_WIN_7_CLASS) {
        startup_info.lpDesktop = TEXT("Winsta0\\default");
        ret = create_process_as_user(_session_id, _agent_path, _agent_path, NULL, NULL, FALSE, 0,
                                     NULL, NULL, &startup_info, &_agent_proc_info);
    } else {
        vd_printf("Not supported in this system version");
        return false;
    }
    if (!ret) {
        vd_printf("CreateProcess() failed: %u", GetLastError());
        return false;
    }
    _agent_alive = true;
    if (_pipe_connected) {
        vd_printf("Pipe already connected");
        return false;
    }
    vd_printf("Wait for vdagent to connect");
    ZeroMemory(&overlap, sizeof(overlap));
    overlap.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    DWORD err = (ConnectNamedPipe(_pipe_state.pipe, &overlap) ? 0 : GetLastError());
    if (err = ERROR_IO_PENDING) {
        DWORD wait_ret = WaitForSingleObject(overlap.hEvent, 3000);
        if (wait_ret != WAIT_OBJECT_0) {
            vd_printf("WaitForSingleObject() failed: %u error: %u", wait_ret,
                wait_ret == WAIT_FAILED ? GetLastError() : 0);
            ret = FALSE;
        }
    } else if (err != 0 || err != ERROR_PIPE_CONNECTED) {
        vd_printf("ConnectNamedPipe() failed: %u", err);
        ret = FALSE;
    }
    if (ret) {
        vd_printf("Pipe connected by vdagent");
        _pipe_connected = true;
        _pending_reset = false;
    }
    CloseHandle(overlap.hEvent);
    return !!ret;
}

bool VDService::kill_agent()
{
    DWORD exit_code = 0;
    DWORD wait_ret;
    HANDLE proc_handle;
    bool ret = true;

    if (!_agent_alive) {
        return true;
    }
    _agent_alive = false;
    proc_handle = _agent_proc_info.hProcess;
    _agent_proc_info.hProcess = 0;
    if (_pipe_connected) {
        _pipe_connected = false;
        DisconnectNamedPipe(_pipe_state.pipe);
    }
    if (GetProcessId(proc_handle)) {
        wait_ret = WaitForSingleObject(proc_handle, 3000);
        switch (wait_ret) {
        case WAIT_OBJECT_0:
            if (GetExitCodeProcess(proc_handle, &exit_code)) {
                vd_printf("vdagent exit code %u", exit_code);
            } else if (exit_code == STILL_ACTIVE) {
                vd_printf("Failed killing vdagent");
                ret = false;
            } else {
                vd_printf("GetExitCodeProcess() failed: %u", GetLastError());
            }
            break;
        case WAIT_TIMEOUT:
            vd_printf("Wait timeout");
            ret = false;
            break;
        case WAIT_FAILED:
        default:
            vd_printf("WaitForSingleObject() failed: %u", GetLastError());
            break;
        }
    }
    CloseHandle(proc_handle);
    CloseHandle(_agent_proc_info.hThread);
    ZeroMemory(&_agent_proc_info, sizeof(_agent_proc_info));
    return ret;
}

bool VDService::restart_agent(bool normal_restart)
{
    DWORD time = GetTickCount();
    bool ret = true;

    MUTEX_LOCK(_agent_mutex);
    if (!normal_restart && ++_agent_restarts > VD_AGENT_MAX_RESTARTS) {
        vd_printf("Agent restarted too many times");
        ret = false;
        stop();
    }
    if (ret && kill_agent() && launch_agent()) {
        if (time - _last_agent_restart_time > VD_AGENT_RESTART_COUNT_RESET_INTERVAL) {
            _agent_restarts = 0;
        }
        _last_agent_restart_time = time;
        ret = true;
        read_pipe();
    }
    MUTEX_UNLOCK(_agent_mutex);
    return ret;
}

void VDService::stop()
{
    vd_printf("Service stopped");
    set_control_event(VD_CONTROL_STOP);
}

void VDService::pipe_write_completion()
{
    VDPipeState* ps = &this->_pipe_state;
    DWORD bytes;

    if (!_running) {
        return;
    }
    if (_pending_write) {
        if (GetOverlappedResult(_pipe_state.pipe, &_pipe_state.write.overlap, &bytes, FALSE)) {
            ps->write.start += bytes;
            if (ps->write.start == ps->write.end) {
                ps->write.start = ps->write.end = 0;
            }
        } else if (GetLastError() == ERROR_IO_PENDING){
            vd_printf("Overlapped write is pending");
            return;
        } else {
            vd_printf("GetOverlappedResult() failed : %d", GetLastError());
        }
        _pending_write = false;
    }

    if (ps->write.start < ps->write.end) {
        _pending_write = true;
        if (!WriteFile(ps->pipe, ps->write.data + ps->write.start,
                       ps->write.end - ps->write.start, NULL, &_pipe_state.write.overlap)) {
            vd_printf("WriteFile() failed: %u", GetLastError());
            _pending_write = false;
            _pipe_connected = false;
            DisconnectNamedPipe(_pipe_state.pipe);
        }
    } else {
        _pending_write = false;
    }
}

void VDService::read_pipe()
{
    VDPipeState* ps = &_pipe_state;
    DWORD bytes;

    if (ps->read.end < sizeof(ps->read.data)) {
        _pending_read = true;
        if (ReadFile(ps->pipe, ps->read.data + ps->read.end, sizeof(ps->read.data) - ps->read.end,
                     &bytes, &ps->read.overlap) || GetLastError() == ERROR_MORE_DATA) {
            _pending_read = false;
            vd_printf("ReadFile without pending %u", bytes);
            handle_pipe_data(bytes);
            read_pipe();
        } else if (GetLastError() != ERROR_IO_PENDING) {
            vd_printf("ReadFile() failed: %u", GetLastError());
            _pending_read = false;
            _pipe_connected = false;
            DisconnectNamedPipe(_pipe_state.pipe);
        }
    } else {
        _pending_read = false;
    }
}

//FIXME: division to max size chunks should be here, not in the agent
void VDService::handle_pipe_data(DWORD bytes)
{
    VDPipeState* ps = &_pipe_state;
    DWORD read_size;

    if (bytes) {
        _pending_read = false;
    }
    if (!_running) {
        return;
    }
    ps->read.end += bytes;
    while (_running && (read_size = ps->read.end - ps->read.start) >= sizeof(VDPipeMessage)) {
        VDPipeMessage* pipe_msg = (VDPipeMessage*)&ps->read.data[ps->read.start];
        if (pipe_msg->type != VD_AGENT_COMMAND) {
            handle_agent_control(pipe_msg);
            ps->read.start += sizeof(VDPipeMessage);
            continue;
        }
        if (read_size < sizeof(VDPipeMessage) + pipe_msg->size) {
            break;
        }
        if (_vdi_port->write_ring_free_space() < sizeof(VDIChunkHeader) + pipe_msg->size) {
            //vd_printf("DEBUG: no space in write ring %u", _vdi_port->write_ring_free_space());
            break;
        }
        if (!_pending_reset) {
            VDIChunkHeader chunk;
            chunk.port = pipe_msg->opaque;
            chunk.size = pipe_msg->size;
            if (_vdi_port->ring_write(&chunk, sizeof(chunk)) != sizeof(chunk) ||
                    _vdi_port->ring_write(pipe_msg->data, chunk.size) != chunk.size) {
                vd_printf("ring_write failed");
                _running = false;
                return;
            }
        }
        ps->read.start += (sizeof(VDPipeMessage) + pipe_msg->size);
    }
    if (ps->read.start == ps->read.end && !_pending_read) {
        DWORD prev_read_end = ps->read.end;
        ps->read.start = ps->read.end = 0;
        if (prev_read_end == sizeof(ps->read.data)) {
            read_pipe();
        }
    }
}

void VDService::handle_port_data()
{
    VDPipeMessage* pipe_msg;
    VDIChunkHeader chunk;
    int chunks_count = 0;
    DWORD count = 0;

    while (_running) {
        if (!_chunk_size && _vdi_port->read_ring_size() >= sizeof(chunk)) {
            if (_vdi_port->ring_read(&chunk, sizeof(chunk)) != sizeof(chunk)) {
                vd_printf("ring_read of chunk header failed");
                _running = false;
                break;
            }
            count = sizeof(VDPipeMessage) + chunk.size;
            if (_pipe_state.write.end + count > sizeof(_pipe_state.write.data)) {
                vd_printf("chunk is too large, size %u port %u", chunk.size, chunk.port);
                _running = false;
                break;
            }
            _chunk_size = chunk.size;
            _chunk_port = chunk.port;
        }
        if (_chunk_size && _vdi_port->read_ring_size() >= _chunk_size) {
            count = sizeof(VDPipeMessage) + _chunk_size;
            ASSERT(_pipe_state.write.end + count <= sizeof(_pipe_state.write.data));
            pipe_msg = (VDPipeMessage*)&_pipe_state.write.data[_pipe_state.write.end];
            if (_vdi_port->ring_read(pipe_msg->data, _chunk_size) != _chunk_size) {
                vd_printf("ring_read of chunk data failed");
                _running = false;
                break;
            }
            if (_pipe_connected) {
                pipe_msg->type = VD_AGENT_COMMAND;
                pipe_msg->opaque = _chunk_port;
                pipe_msg->size = _chunk_size;
                _pipe_state.write.end += count;
                chunks_count++;
            } else {
                _pipe_state.write.start = _pipe_state.write.end = 0;
            }
            _chunk_size = 0;
            _chunk_port = 0;
        } else {
            break;
        }
    }
    if (_pipe_connected && chunks_count && !_pending_write) {
        pipe_write_completion();
    }
}

bool VDService::handle_agent_control(VDPipeMessage* msg)
{
    switch (msg->type) {
    case VD_AGENT_RESET_ACK: {
        if (msg->opaque != _connection_id) {
            vd_printf("Agent reset ack mismatch %u %u", msg->opaque, _connection_id);
            break;
        }
        vd_printf("Agent reset ack");
        _pending_reset = false;
        break;
    }
    default:
        vd_printf("Unsupported control %u %u", msg->type, msg->opaque);
        return false;
    }
    return true;
}

void VDService::write_agent_control(uint32_t type, uint32_t opaque)
{
    if (!_pipe_connected) {
        return;
    }
    if (_pipe_state.write.end + sizeof(VDPipeMessage) > sizeof(_pipe_state.write.data)) {
        vd_printf("msg is too large");
        _running = false;
        return;
    }
    VDPipeMessage* msg = (VDPipeMessage*)&_pipe_state.write.data[_pipe_state.write.end];
    msg->type = type;
    msg->opaque = opaque;
    _pipe_state.write.end += sizeof(VDPipeMessage);
    if (!_pending_write) {
        pipe_write_completion();
    }
}

int _tmain(int argc, TCHAR* argv[], TCHAR* envp[])
{
    bool success = false;

    if (!supported_system_version()) {
        printf("vdservice is not supported in this system version\n");
        return -1;
    }
    VDService* vdservice = VDService::get();
    if (argc > 1) {
        if (lstrcmpi(argv[1], TEXT("install")) == 0) {
            success = vdservice->install();
        } else if (lstrcmpi(argv[1], TEXT("uninstall")) == 0) {
            success = vdservice->uninstall();
        } else {
            printf("Use: vdservice install / uninstall\n");
        }
    } else {
        success = vdservice->run();
    }
    delete vdservice;
    return (success ? 0 : -1);
}
