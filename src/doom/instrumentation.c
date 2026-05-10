#include "instrumentation.h"
#include "doomtype.h"

#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int ins_socket = -1;
static boolean ins_net_init = false;

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