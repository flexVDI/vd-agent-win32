/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include <algorithm>
#include <winsock2.h>
#include <winsock.h>
#include <mswsock.h>
#include <tchar.h>
#include "port_forward.h"

static const char* getErrorMessage(DWORD error)
{
    static TCHAR errmsg[512];
    static char cerrmsg[512];
    if (!FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, error, 0, errmsg, 511, NULL)) {
        return (getErrorMessage(GetLastError()));
    }
#ifdef _UNICODE
    WideCharToMultiByte(CP_UTF8, 0, errmsg, -1, cerrmsg, 512, NULL, NULL);
    return cerrmsg;
#else
    return errmsg;
#endif
}

struct ScopedLock {
    mutex_t &mutex;
    ScopedLock(mutex_t &m) : mutex(m) {
        MUTEX_LOCK(mutex);
    }
    ~ScopedLock() {
        MUTEX_UNLOCK(mutex);
    }
};

struct Buffer {
    uint8_t *buff;
    size_t size, pos;

    Buffer() : buff(NULL), size(0), pos(0) {}
    ~Buffer() {
        if (buff) delete[] buff;
    }
    void copy_data(const uint8_t * data, size_t s) {
        if (buff) delete[] buff;
        buff = new uint8_t[size = s];
        pos = 0;
        std::copy(data, data + s, buff);
    }
    void getWSABuffer(WSABUF* buffer) {
        buffer->len = size - pos;
        buffer->buf = (char *)&buff[pos];
    }
};

struct Connection {
    PortForwarder::conn_id_t id;
    SOCKET sock;
    bool closing;
    std::list<Buffer> write_buffer;
    uint32_t data_sent, data_received, ack_interval;
    static const uint32_t WINDOW_SIZE = 10*1024*1024;

    Connection() : sock(INVALID_SOCKET), closing(false), data_sent(0),
        data_received(0), ack_interval(0) {}
    ~Connection() {
        if (sock != INVALID_SOCKET) {
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        }
        // TODO Cancel all pending operations on this connection
    }

    static PortForwarder::conn_id_t getId(SOCKET sock);
    void add_data_to_write_buffer(const uint8_t * data, size_t size) {
        write_buffer.push_back(Buffer());
        write_buffer.back().copy_data(data, size);
    }
    void getWSABuffer(WSABUF *buffer) {
        write_buffer.front().getWSABuffer(buffer);
    }
};

PortForwarder::conn_id_t Connection::getId(SOCKET sock)
{
    SOCKADDR_IN addr;
    int addr_len = sizeof(addr);
    if (!getpeername(sock, (SOCKADDR *)&addr, &addr_len)) {
        return ntohs(addr.sin_port);
    } else {
        LOG(LOG_DEBUG, "getpeername on socket %d failed: %s", sock, getErrorMessage(WSAGetLastError()));
        return -1;
    }
}

struct Acceptor {
    static const size_t sizeOfAddress = sizeof(SOCKADDR_IN) + 16;
    static const GUID acceptex_guid, get_sockaddr_guid;

    PortForwarder::port_t port;
    SOCKET sock;
    uint8_t addrBuffer[sizeOfAddress * 2];

    Acceptor() : sock(INVALID_SOCKET) {}
    ~Acceptor() {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
        }
        // TODO Cancel all pending operations on this connection
    }

    bool accept_ex(SOCKET accSock, LPOVERLAPPED pOverlapped);
    void get_accept_sockaddrs(LPSOCKADDR_IN *localAddr, LPSOCKADDR_IN *remoteAddr);
};
const GUID Acceptor::acceptex_guid = WSAID_ACCEPTEX;
const GUID Acceptor::get_sockaddr_guid = WSAID_GETACCEPTEXSOCKADDRS;

static void* load_function(SOCKET sock, GUID guid) {
    void* result = NULL;
    DWORD dwBytes = 0;

    WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(GUID),
             &result, sizeof(void *), &dwBytes, 0, 0);
    return result;
}

bool Acceptor::accept_ex(SOCKET accSock, LPOVERLAPPED pOverlapped)
{
    static LPFN_ACCEPTEX real_accept_ex =
        (LPFN_ACCEPTEX)load_function(sock, acceptex_guid);
    DWORD bytes;
    if (real_accept_ex) {
        LOG(LOG_DEBUG, "Calling accept_ex for operation %p", this);
        return real_accept_ex(sock, accSock, addrBuffer, 0,
                              sizeOfAddress, sizeOfAddress, &bytes, pOverlapped);
    } else {
        WSASetLastError(WSASYSCALLFAILURE);
        return false;
    }
}

void Acceptor::get_accept_sockaddrs(LPSOCKADDR_IN *localAddr, LPSOCKADDR_IN *remoteAddr)
{
    static LPFN_GETACCEPTEXSOCKADDRS real_get_accept_sockaddrs =
        (LPFN_GETACCEPTEXSOCKADDRS)load_function(sock, get_sockaddr_guid);
    int addrLen = sizeof(SOCKADDR_IN);
    if (real_get_accept_sockaddrs) {
        real_get_accept_sockaddrs(addrBuffer, 0, sizeOfAddress, sizeOfAddress,
                                  (LPSOCKADDR *)localAddr, &addrLen,
                                  (LPSOCKADDR *)remoteAddr, &addrLen);
    } else {
        WSASetLastError(WSASYSCALLFAILURE);
    }
}

struct OverlappedOperation : public OVERLAPPED {
    OverlappedOperation() {
        uint8_t *p = (uint8_t *)static_cast<OVERLAPPED *>(this);
        std::fill(p, p + sizeof(OVERLAPPED), 0);
    }
    virtual ~OverlappedOperation() {}
    virtual void handle_to(PortForwarder &pf, DWORD bytes) = 0;
    bool check_pending() {
        const DWORD lastError = ::WSAGetLastError();
        if (lastError != ERROR_IO_PENDING) {
            LOG(LOG_WARN, "Overlapped IO error: %s", getErrorMessage(lastError));
            return false;
        } else {
            LOG(LOG_DEBUG, "Pending io operation %p", this);
            return true;
        }
    }
};

struct AcceptOperation : public OverlappedOperation {
    uint16_t port;
    SOCKET client;

    AcceptOperation() : OverlappedOperation() {
        client = socket(AF_INET, SOCK_STREAM, 0);
        LOG(LOG_DEBUG, "Created accept operation %p for client socket %d", this, client);
    }
    virtual ~AcceptOperation() {
        if (client != INVALID_SOCKET) {
            LOG(LOG_DEBUG, "Closing unaccepted client in operation %p for client socket %d",
                this, client);
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
    virtual void handle_to(PortForwarder &pf, DWORD bytes) {
        pf.handle_accept(port, client);
        client = INVALID_SOCKET;
    }
    static bool post(Acceptor & acceptor, HANDLE iocp)
    {
        AcceptOperation *operation = new AcceptOperation;
        operation->port = acceptor.port;
        if(!acceptor.accept_ex(operation->client, operation)) {
            if(!operation->check_pending()) {
                // TODO: Error
                LOG(LOG_DEBUG, "Posting accept operation %p failed", operation);
                delete operation;
                return false;
            }
        } else {
            // Operation completed synchronously, post data to io thread.
            LOG(LOG_DEBUG, "Accept operation %p completed synchronously, posting", operation);
            ::PostQueuedCompletionStatus(iocp, 0, 0, operation);
        }
        return true;
    }
};

struct ReadOperation : public OverlappedOperation {
    static const size_t MAX_MSG_SIZE = VD_AGENT_MAX_DATA_SIZE - sizeof(VDAgentMessage);
    static const size_t DATA_HEAD_SIZE = sizeof(VDAgentPortForwardDataMessage);
    static const size_t BUFFER_SIZE = MAX_MSG_SIZE - DATA_HEAD_SIZE;
    int id;
    char *data_msg_buffer;
    WSABUF buffer[1];

    ReadOperation(char *b) : OverlappedOperation(), data_msg_buffer(b) {
        buffer->len = BUFFER_SIZE;
        buffer->buf = data_msg_buffer + DATA_HEAD_SIZE;
        LOG(LOG_DEBUG, "Created read operation %p", this);
    }
    virtual ~ReadOperation() {}
    virtual void handle_to(PortForwarder &pf, DWORD bytes) {
        pf.handle_read(id, (VDAgentPortForwardDataMessage *)data_msg_buffer, bytes);
    }
    static bool post(Connection & conn, HANDLE iocp, PortForwarder::Sender &sender) {
        char *read_buffer = (char *)sender.get_buffer(MAX_MSG_SIZE);
        ReadOperation *operation = new ReadOperation(read_buffer);
        operation->id = conn.id;
        DWORD f = 0;
        if (WSARecv(conn.sock, operation->buffer,
                    1, NULL, &f, operation, NULL) == SOCKET_ERROR) {
            if (!operation->check_pending()) {
                // TODO: Error
                LOG(LOG_DEBUG, "Posting read operation %p failed", operation);
                delete operation;
                return false;
            }
        } else
            LOG(LOG_DEBUG, "Read operation %p completed synchronously", operation);
        return true;
    }
};

struct WriteOperation : public OverlappedOperation {
    int id;
    WSABUF buffer[1];

    WriteOperation() : OverlappedOperation() {
        LOG(LOG_DEBUG, "Created write operation %p", this);
    }
    virtual ~WriteOperation() {}
    virtual void handle_to(PortForwarder &pf, DWORD bytes) {
        pf.handle_write(id, bytes);
    }
    static bool post(Connection & conn, HANDLE iocp) {
        WriteOperation *operation = new WriteOperation;
        operation->id = conn.id;
        conn.getWSABuffer(operation->buffer);
        if(WSASend(conn.sock, operation->buffer,
                   1, NULL, 0, operation, NULL) == SOCKET_ERROR) {
            if (!operation->check_pending()) {
                // TODO: Error
                LOG(LOG_DEBUG, "Posting write operation %p failed", operation);
                delete operation;
                return false;
            }
        } else
            LOG(LOG_DEBUG, "Write operation %p completed synchronously", operation);
        return true;
    }
};

PortForwarder::PortForwarder(Sender& s)
    : sender(s), has_client(true)
{
    WSADATA WsaDat;
    // TODO: Check for errors and throw
    WSAStartup(MAKEWORD(2, 2), &WsaDat);
    iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    iocpThread = ::CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
    MUTEX_INIT(mutex);
}

PortForwarder::~PortForwarder()
{
    LOG(LOG_INFO, "Client disconnected, removing port redirections");
    {
        ScopedLock lock(mutex);
        has_client = false;
        connections.clear();
        acceptors.clear();
    }
    WaitForSingleObject(iocpThread, INFINITE);
    CloseHandle(iocpThread);
    CloseHandle(iocp);
    WSACleanup();
}

void PortForwarder::handle_io_events()
{
    LOG(LOG_INFO, "Starting port forwarding thread.");
    while (has_client) {
        DWORD bytes;
        ULONG key;
        LPOVERLAPPED overlapped = NULL;
        bool success = GetQueuedCompletionStatus(iocp, &bytes, &key, &overlapped, INFINITE);
        OverlappedOperation * operation = static_cast<OverlappedOperation *>(overlapped);
        if (!success || !operation) {
            // Operation failed (probably canceled)
            LOG(LOG_DEBUG, "IO operation %p failed: %s", operation,
                getErrorMessage(WSAGetLastError()));
        } else {
            LOG(LOG_DEBUG, "IO operation %p finished", operation);
            ScopedLock lock(mutex);
            operation->handle_to(*this, bytes);
        }
        delete operation;
    }
    LOG(LOG_INFO, "Ending port forwarding thread.");
}

void PortForwarder::handle_accept(uint16_t port, SOCKET client)
{
    accept_iter acceptit = acceptors.find(port);
    if (acceptit == acceptors.end()) {
        LOG(LOG_ERROR, "Unknown port %d in operation %p", port, this);
        // TODO: Error
        return;
    }
    Acceptor &acceptor = acceptit->second;
    char update_context[sizeof(DWORD)];
    *((DWORD *)update_context) = acceptor.sock;
    setsockopt(client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, update_context, sizeof(DWORD));
    conn_id_t id = Connection::getId(client);
    if (id != -1) {
        LOG(LOG_DEBUG, "Connection %d accepted on port %d", id, port);
        Connection &conn = connections[id];
        conn.sock = client;
        conn.id = id;
        CreateIoCompletionPort((HANDLE)client, iocp, 0, 0);
        VDAgentPortForwardConnectMessage *msg =
            sender.get_buffer<VDAgentPortForwardConnectMessage>();
        msg->port = port;
        msg->id = id;
        msg->ack_interval = Connection::WINDOW_SIZE/2;
        sender.send(VD_AGENT_PORT_FORWARD_CONNECT, msg);
    } else {
        LOG(LOG_DEBUG, "Cannont obtain id of connection accepted on port %d", port);
        // TODO: Error
    }
    AcceptOperation::post(acceptor, iocp);
}

void PortForwarder::handle_read(int id, VDAgentPortForwardDataMessage *msg, DWORD bytes)
{
    conn_iter connit = connections.find(id);
    if (connit == connections.end()) {
        LOG(LOG_ERROR, "Unknown connection %d in operation %p", id, this);
        // TODO: Error
        return;
    }
    if (bytes == 0) {
        // Connection closed by peer
        VDAgentPortForwardCloseMessage *closeMsg =
            sender.get_buffer<VDAgentPortForwardCloseMessage>();
        closeMsg->id = id;
        sender.send(VD_AGENT_PORT_FORWARD_CLOSE, closeMsg);
        connections.erase(connit);
    } else if (!connit->second.closing) {
        Connection &conn = connit->second;
        LOG(LOG_DEBUG, "%d bytes read on connection %d", bytes, id);
        msg->id = id;
        msg->size = bytes;
        sender.send(VD_AGENT_PORT_FORWARD_DATA, bytes + ReadOperation::DATA_HEAD_SIZE, msg);
        conn.data_sent += bytes;
        if (conn.data_sent < Connection::WINDOW_SIZE) {
            if (!ReadOperation::post(conn, iocp, sender)) {
                // TODO: Error
            }
        }
    }
}

void PortForwarder::handle_write(int id, DWORD bytes)
{
    conn_iter connit = connections.find(id);
    if (connit == connections.end()) {
        LOG(LOG_ERROR, "Unknown connection %d in operation %p", id, this);
        // TODO: Error
        return;
    }
    if (bytes == 0) {
        LOG(LOG_DEBUG, "We read 0 bytes in connection %d", id);
        // TODO: is this a closed connection??
    } else {
        Connection &conn = connit->second;
        conn.data_received += bytes;
        if (conn.data_received >= conn.ack_interval) {
            VDAgentPortForwardAckMessage *ackMsg =
                sender.get_buffer<VDAgentPortForwardAckMessage>();
            ackMsg->id = id;
            ackMsg->size = conn.data_received;
            conn.data_received = 0;
            sender.send(VD_AGENT_PORT_FORWARD_ACK, ackMsg);
        }
        Buffer &buffer = conn.write_buffer.front();
        buffer.pos += bytes;
        if (buffer.pos == buffer.size) {
            conn.write_buffer.pop_front();
        }
        if (!conn.write_buffer.empty()) {
            if (!WriteOperation::post(conn, iocp)) {
                // TODO: Error
            }
        } else if (conn.closing) {
            connections.erase(connit);
        }
    }
}

void PortForwarder::remote_connected(VDAgentPortForwardConnectMessage& msg)
{
    conn_iter connit = connections.find(msg.id);
    if (connit == connections.end()) {
        LOG(LOG_ERROR, "Unknown connection %d from client CONNECT", msg.id);
        // TODO: Error
    } else {
        Connection &conn = connit->second;
        conn.ack_interval = msg.ack_interval;
        // Program a read operation
        if (!ReadOperation::post(conn, iocp, sender)) {
            // TODO: Error
        }
    }
}

void PortForwarder::ack_data(VDAgentPortForwardAckMessage& msg)
{
    conn_iter connit = connections.find(msg.id);
    if (connit == connections.end()) {
        LOG(LOG_ERROR, "Unknown connection %d from client ACK", msg.id);
        // TODO: Error
    } else {
        Connection &conn = connit->second;
        uint32_t data_sent_before = conn.data_sent;
        conn.data_sent -= msg.size;
        if (conn.data_sent < Connection::WINDOW_SIZE &&
            data_sent_before >= Connection::WINDOW_SIZE) {
            // Program a read operation
            if (!ReadOperation::post(conn, iocp, sender)) {
                // TODO: Error
            }
        }
    }
}

void PortForwarder::start_closing(VDAgentPortForwardCloseMessage& msg)
{
    conn_iter connit = connections.find(msg.id);
    if (connit == connections.end()) {
        LOG(LOG_ERROR, "Unknown connection %d from client", msg.id);
        // TODO: Error
    } else {
        Connection &conn = connit->second;
        LOG(LOG_DEBUG, "Client closed connection %d", msg.id);
        if (!conn.write_buffer.empty()) {
            conn.closing = true;
        } else {
            connections.erase(connit);
        }
    }
}

void PortForwarder::listen_to(port_t port)
{
    SOCKET sock;
    char true_placeholder[sizeof(BOOL)];
    SOCKADDR_IN addr;

    if (acceptors.find(port) != acceptors.end()) {
        LOG(LOG_INFO, "Already listening to port %d", (int)port);
    } else {
        *((BOOL *)true_placeholder) = 1;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        // TODO: set zero TCP buffer
        if (sock == INVALID_SOCKET ||
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, true_placeholder, sizeof(BOOL)) ||
            bind(sock, (LPSOCKADDR) &addr, sizeof(addr)) == SOCKET_ERROR ||
            //setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, true_placeholder, sizeof(BOOL)) ||
            listen(sock, 5)) {
            LOG(LOG_ERROR, "Failed to listen to port %d: %s", port,
                getErrorMessage(WSAGetLastError()));
        } else {
            CreateIoCompletionPort((HANDLE)sock, iocp, 0, 0);
            Acceptor &acceptor = acceptors[port];
            acceptor.sock = sock;
            acceptor.port = port;
            LOG(LOG_DEBUG, "Listening to port %d", (int)port);
            if (!AcceptOperation::post(acceptor, iocp)) {
                // TODO: Error
            }
        }
    }
}

void PortForwarder::send_data(const VDAgentPortForwardDataMessage& msg)
{
    if (msg.size) {
        conn_iter it = connections.find(msg.id);
        if (it != connections.end()) {
            Connection & conn = it->second;
            conn.add_data_to_write_buffer(msg.data, msg.size);
            if (conn.write_buffer.size() == 1) {
                if (!WriteOperation::post(conn, iocp)) {
                    // TODO: Error
                }
            }
        }
        /* Ignore unknown connections, they happen when data messages
         * arrive before the close command has reached the client.
         */
    }
}

void PortForwarder::shutdown_port(uint16_t port)
{
    if (port == 0) {
        LOG(LOG_DEBUG, "Resetting port forwarder by client");
        connections.clear();
        acceptors.clear();
    } else if (!acceptors.erase(port)) {
        LOG(LOG_WARN, "Not listening to port %d on shutdown command", port);
    }
}

bool PortForwarder::dispatch(uint32_t command, void* data)
{
    LOG(LOG_DEBUG, "Receiving command %d", (int)command);
    has_client = true;
    ScopedLock lock(mutex);
    switch (command) {
        case VD_AGENT_PORT_FORWARD_LISTEN:
            listen_to(((VDAgentPortForwardListenMessage *)data)->port);
            break;
        case VD_AGENT_PORT_FORWARD_CONNECT:
            remote_connected(*(VDAgentPortForwardConnectMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_DATA:
            send_data(*(VDAgentPortForwardDataMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_ACK:
            ack_data(*(VDAgentPortForwardAckMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_CLOSE:
            start_closing(*(VDAgentPortForwardCloseMessage *)data);
            break;
        case VD_AGENT_PORT_FORWARD_SHUTDOWN:
            shutdown_port(((VDAgentPortForwardShutdownMessage *)data)->port);
            break;
        default:
            has_client = false;
            LOG(LOG_WARN, "Unknown command %d\n", (int)command);
            return false;
    }
    return true;
}
