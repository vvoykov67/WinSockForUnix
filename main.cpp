#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "WinSockForUnix.h"

// Custom structure embedding OVERLAPPED (user context)
typedef struct _OperationContext {
    OVERLAPPED ovl;
    int sock;
    WSABUF buffers[MAX_BUFFERS];
    DWORD buf_count;
    DWORD bytes_transferred;
    OperationType op_type;
    int error_code;
    int complete;
    char *buffer_data;
} OperationContext;

int main() {
    // Create two TCP sockets
    int sock1 = socket(AF_INET, SOCK_STREAM, 0);
    int sock2 = socket(AF_INET, SOCK_STREAM, 0);
    if (sock1 == INVALID_SOCKET || sock2 == INVALID_SOCKET) {
        printf("socket failed: %d\n", MapErrnoToWSAError(errno));
        return 1;
    }

    // Set non-blocking
    if (SetNonBlocking(sock1) == SOCKET_ERROR || SetNonBlocking(sock2) == SOCKET_ERROR) {
        printf("fcntl failed: %d\n", MapErrnoToWSAError(errno));
        close(sock1);
        close(sock2);
        return 1;
    }

    // Server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        printf("inet_pton failed: %s\n", strerror(errno));
        close(sock1);
        close(sock2);
        return 1;
    }

    // Connect sockets
    if (connect(sock1, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        if (errno != EINPROGRESS) {
            printf("connect sock1 failed: %d\n", MapErrnoToWSAError(errno));
            close(sock1);
            close(sock2);
            return 1;
        }
    }
    if (connect(sock2, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        if (errno != EINPROGRESS) {
            printf("connect sock2 failed: %d\n", MapErrnoToWSAError(errno));
            close(sock1);
            close(sock2);
            return 1;
        }
    }

    // Prepare send operation (sock1)
    OperationContext *send_op = (OperationContext *)calloc(1, sizeof(OperationContext));
    if (!send_op) {
        printf("calloc failed for send_op\n");
        close(sock1);
        close(sock2);
        return 1;
    }
    send_op->sock = sock1;
    send_op->op_type = OP_SEND;
    send_op->ovl.hEvent = WSACreateEvent();
    if (!send_op->ovl.hEvent) {
        printf("WSACreateEvent failed\n");
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }
    char send_buf1[] = "Hello, ";
    char send_buf2[] = "Unix!";
    send_op->buffers[0].buf = send_buf1;
    send_op->buffers[0].len = strlen(send_buf1);
    send_op->buffers[1].buf = send_buf2;
    send_op->buffers[1].len = strlen(send_buf2);
    send_op->buf_count = 2;

    DWORD bytes_sent = 0;
    int ret = WSASend(sock1, send_op->buffers, 2, &bytes_sent, &send_op->ovl);
    if (ret == SOCKET_ERROR && errno != WSA_IO_PENDING) {
        printf("WSASend failed: %d\n", MapErrnoToWSAError(errno));
        WSACloseEvent(send_op->ovl.hEvent);
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }

    // Prepare receive operation (sock2)
    OperationContext *recv_op = (OperationContext *)calloc(1, sizeof(OperationContext));
    if (!recv_op) {
        printf("calloc failed for recv_op\n");
        WSACloseEvent(send_op->ovl.hEvent);
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }
    recv_op->sock = sock2;
    recv_op->op_type = OP_RECEIVE;
    recv_op->ovl.hEvent = WSACreateEvent();
    if (!recv_op->ovl.hEvent) {
        printf("WSACreateEvent failed\n");
        free(recv_op);
        WSACloseEvent(send_op->ovl.hEvent);
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }
    recv_op->buffer_data = (char *)calloc(32, sizeof(char));
    if (!recv_op->buffer_data) {
        printf("calloc failed for recv buffer\n");
        WSACloseEvent(recv_op->ovl.hEvent);
        free(recv_op);
        WSACloseEvent(send_op->ovl.hEvent);
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }
    recv_op->buffers[0].buf = recv_op->buffer_data;
    recv_op->buffers[0].len = 16;
    recv_op->buffers[1].buf = recv_op->buffer_data + 16;
    recv_op->buffers[1].len = 16;
    recv_op->buf_count = 2;

    DWORD bytes_received = 0;
    DWORD flags = 0;
    ret = WSARecv(sock2, recv_op->buffers, 2, &bytes_received, &flags, &recv_op->ovl);
    if (ret == SOCKET_ERROR && errno != WSA_IO_PENDING) {
        printf("WSARecv failed: %d\n", MapErrnoToWSAError(errno));
        WSACloseEvent(recv_op->ovl.hEvent);
        free(recv_op->buffer_data);
        free(recv_op);
        WSACloseEvent(send_op->ovl.hEvent);
        free(send_op);
        close(sock1);
        close(sock2);
        return 1;
    }

    // Set up events array
    WSAEVENT events[2] = {send_op->ovl.hEvent, recv_op->ovl.hEvent};
    unsigned long event_count = 2;

    // Wait for operations (wait-any, 5-second timeout)
    unsigned long wait_result = WaitForMultipleEvents(event_count, events, 0, 5000);
    if (wait_result == WAIT_TIMEOUT) {
        printf("WaitForMultipleEvents timed out\n");
    } else if (wait_result == WAIT_FAILED) {
        printf("WaitForMultipleEvents failed: %d\n", MapErrnoToWSAError(errno));
    } else {
        int completed = 0;
        for (unsigned long i = 0; i < event_count; i++) {
            InternalContext *internal_ctx = (InternalContext *)events[i]->internal_ctx;
            if (!internal_ctx || internal_ctx->complete) {
                completed++;
                continue;
            }
            unsigned long bytes_transferred = 0;
            int result = GetOverlappedResult(internal_ctx->sock, internal_ctx->ovl, &bytes_transferred, 0);
            if (!result && internal_ctx->error_code) {
                internal_ctx->complete = 1;
            }
            if (internal_ctx->complete) {
                completed++;
                printf("%s operation: %lu bytes transferred, error: %d\n",
                       internal_ctx->op_type == OP_SEND ? "Send" : "Receive",
                       bytes_transferred, internal_ctx->error_code);
                if (internal_ctx->op_type == OP_RECEIVE) {
                    printf("Buffer 1: %s\n", internal_ctx->buffers[0].buf);
                    printf("Buffer 2: %s\n", internal_ctx->buffers[1].buf);
                }
            }
        }
        printf("Completed %d operations\n", completed);
    }

    // Clean up
    WSACloseEvent(send_op->ovl.hEvent);
    free(send_op);
    WSACloseEvent(recv_op->ovl.hEvent);
    free(recv_op->buffer_data);
    free(recv_op);
    close(sock1);
    close(sock2);
    return 0;
}
