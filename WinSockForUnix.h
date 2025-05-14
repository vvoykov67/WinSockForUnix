#pragma once

#include <errno.h>
#include <sys/socket.h>

#define MAX_BUFFERS 2
#define MAX_OPERATIONS 16
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)

// Winsock-like error codes
#define WSA_IO_PENDING 997
#define WSAEWOULDBLOCK 10035
#define WSAEINVAL 10022
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WAIT_FAILED 0xFFFFFFFF
#define WAIT_TIMEOUT 258
#define WAIT_OBJECT_0 0x00000000

// Windows-like types
typedef unsigned long DWORD;

// Simulate Windows WSABUF
typedef struct {
    DWORD len;
    char *buf;
} WSABUF;

// Simulate Windows WSAEVENT
typedef struct _WSAEvent {
    int eventfd;                // Event file descriptor
    struct _InternalContext *internal_ctx; // Internal context
} WSAEvent;

typedef WSAEvent *WSAEVENT;

// Simulate Windows OVERLAPPED
typedef struct _OVERLAPPED {
    DWORD Internal;
    DWORD InternalHigh;
    DWORD Offset;
    DWORD OffsetHigh;
    WSAEVENT hEvent;
} OVERLAPPED;

// Operation type
typedef enum {
    OP_SEND,
    OP_RECEIVE
} OperationType;

// Internal context for operation state
typedef struct _InternalContext {
    int sock;
    WSABUF buffers[MAX_BUFFERS];
    DWORD buf_count;
    DWORD current_buffer;
    DWORD bytes_transferred;
    DWORD offset;
    OperationType op_type;
    int error_code;
    int complete;
    OVERLAPPED *ovl; // Reference to the OVERLAPPED structure
} InternalContext;

WSAEVENT WSACreateEvent();
int WSACloseEvent(WSAEVENT event);
int WSASend(int sockfd, WSABUF *buffers, DWORD buf_count, DWORD *bytes_sent, OVERLAPPED *ovl);
int WSARecv(int sockfd, WSABUF *buffers, DWORD buf_count, DWORD *bytes_received, DWORD *flags, OVERLAPPED *ovl);
int GetOverlappedResult(int sockfd, OVERLAPPED *ovl, DWORD *bytes_transferred, int wait);
unsigned long WaitForMultipleEvents(unsigned long event_count, WSAEVENT *events, int wait_all, unsigned long timeout_ms);
int SetNonBlocking(int sockfd);
// Map Unix errno to Winsock error codes
int MapErrnoToWSAError(int err);
