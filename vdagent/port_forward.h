/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#ifndef __PORT_FORWARD_H
#define __PORT_FORWARD_H

#include <map>
#include <list>
#include "vdcommon.h"

struct Connection;
struct Acceptor;

class PortForwarder {
public:
    typedef uint16_t port_t;
    typedef int conn_id_t;
    typedef std::map<conn_id_t, Connection>::iterator conn_iter;
    typedef std::map<port_t, Acceptor>::iterator accept_iter;

    class Sender {
    public:
        virtual void send(uint32_t type, size_t size, void* data) = 0;
        virtual void *get_buffer(size_t size) = 0;
        template <typename T> T *get_buffer() {
            return (T *)get_buffer(sizeof(T));
        }
        template <typename T> void send(uint32_t type, T* data) {
            send(type, sizeof(T), data);
        }
    };

    PortForwarder(Sender& cb);
    ~PortForwarder();

    // Main thread methods
    bool dispatch(uint32_t command, void* data);

    // Event thread callbacks
    void handle_accept(uint16_t port, SOCKET client);
    void handle_read(int id, VDAgentPortForwardDataMessage *msg, DWORD bytes);
    void handle_write(int id, DWORD bytes);
    void handle_connect(int id);

private:
    Sender& sender;
    std::map<port_t, Acceptor> acceptors;
    std::map<conn_id_t, Connection> connections;
    bool has_client;
    HANDLE iocp;
    HANDLE iocpThread;
    mutex_t mutex;

    void handle_io_events();
    static DWORD WINAPI ThreadProc(LPVOID lpParameter) {
        try {
            ((PortForwarder *)lpParameter)->handle_io_events();
        } catch (...) {}
        return 0;
    }

    void listen_to(VDAgentPortForwardListenMessage &msg);
    void send_data(const VDAgentPortForwardDataMessage &msg);
    void remote_connected(VDAgentPortForwardConnectMessage &msg);
    void connect_remote(VDAgentPortForwardConnectMessage &msg);
    void ack_data(VDAgentPortForwardAckMessage &msg);
    void start_closing(VDAgentPortForwardCloseMessage &msg);
    void shutdown_port(uint16_t port);
};

#endif // __PORT_FORWARD_H
