#include "instrumentation.h"
#include "doomtype.h"

#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int ins_socket = -1;
static boolean ins_net_init = false;

ins_outbound_packet_t ins_outbound;

void INS_Init(const char *host, int port)
{
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    printf("[INS] Initializing network connection to %s:%d\n", host, port);

    ins_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(ins_socket < 0)
    {
        printf("[INS] Failed to create socket, exiting\n");
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        printf("[INS] Invalid address\n");
        close(ins_socket);
        return;
    }

    if (connect(ins_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[INS] Connection failed (data server not running?)\n");
        close(ins_socket);
        ins_socket = -1;
        return;
    }

    ins_net_init = true;
    printf("[INS] Init successful, connected to data server\n");
}

ssize_t send_all(int sock, const void *buf, size_t len)
{
    size_t total = 0;
    const char *p = buf;

    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);

        if (n < 0) {
            return -1;
        }

        total += n;
    }

    return total;
}

boolean check_sock_alive()
{
    char buf;
    int ret = recv(ins_socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);

    if (ret > 0) {
        return true;
    }
    else if (ret == 0) {
        return false;
    }
    else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        } else {
            return false;
        }
    }
}

void INS_BeginFrame() {
    memset(&ins_outbound, 0, sizeof(ins_outbound_packet_t));
}

void INS_EndTransmit()
{
    if(ins_net_init && !check_sock_alive())
    {
        ins_net_init = false;
        fprintf(stderr, "[INS] Socket has become invalid\n");
        return;
    }
    if(ins_net_init && send_all(ins_socket, &ins_outbound, sizeof(ins_outbound_packet_t)) < 0)
    {
        fprintf(stderr, "[INS] Failed to send data to server\n");
    }
}