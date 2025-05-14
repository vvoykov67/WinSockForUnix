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

// Map Unix errno to Winsock error codes
int MapErrnoToWSAError(int err) {
    switch (err) {
        case EAGAIN: return WSAEWOULDBLOCK;
        case EINVAL: return WSAEINVAL;
        case ETIMEDOUT: return WSAETIMEDOUT;
        case ECONNREFUSED: return WSAECONNREFUSED;
        default: return err;
    }
}

// Helper to set socket non-blocking
int SetNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) return SOCKET_ERROR;
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// Convert WSABUF to iovec
void WSABUFToIovec(WSABUF *wsabuf, DWORD buf_count, struct iovec *iov) {
    for (DWORD i = 0; i < buf_count; i++) {
        iov[i].iov_base = wsabuf[i].buf;
        iov[i].iov_len = wsabuf[i].len;
    }
}

// Create WSAEVENT
WSAEVENT WSACreateEvent() {
    WSAEvent *event = (WSAEvent *)calloc(1, sizeof(WSAEvent));
    if (!event) return NULL;
    event->eventfd = eventfd(0, EFD_NONBLOCK);
    if (event->eventfd < 0) {
        free(event);
        return NULL;
    }
    return event;
}

// Close WSAEVENT
int WSACloseEvent(WSAEVENT event) {
    if (!event) return 0;
    if (event->internal_ctx) free(event->internal_ctx);
    if (event->eventfd > 0) close(event->eventfd);
    free(event);
    return 1;
}

// Simulate WSASend
int WSASend(int sockfd, WSABUF *buffers, DWORD buf_count, DWORD *bytes_sent, OVERLAPPED *ovl) {
    InternalContext *internal_ctx = NULL;

    if (!buffers || buf_count <= 0 || buf_count > MAX_BUFFERS || !bytes_sent || !ovl || !ovl->hEvent) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    // Check if OVERLAPPED is already in use for a pending operation
    if (ovl->hEvent->internal_ctx && !((InternalContext *)ovl->hEvent->internal_ctx)->complete) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (ovl->hEvent->internal_ctx) {
        internal_ctx = (InternalContext *)ovl->hEvent->internal_ctx;
        // Reset fields for reuse
        internal_ctx->bytes_transferred = 0;
        internal_ctx->complete = 0;
        internal_ctx->error_code = 0;
        internal_ctx->current_buffer = 0;
        internal_ctx->offset = 0;
        internal_ctx->sock = sockfd;
        internal_ctx->op_type = OP_SEND;
        for (DWORD i = 0; i < buf_count; i++) {
            internal_ctx->buffers[i].buf = buffers[i].buf;
            internal_ctx->buffers[i].len = buffers[i].len;
        }
        internal_ctx->buf_count = buf_count;
        internal_ctx->ovl = ovl;
    } else {
        internal_ctx = (InternalContext *)calloc(1, sizeof(InternalContext));
        if (!internal_ctx) {
            errno = WSAEINVAL;
            return SOCKET_ERROR;
        }
        internal_ctx->sock = sockfd;
        internal_ctx->op_type = OP_SEND;
        for (DWORD i = 0; i < buf_count; i++) {
            internal_ctx->buffers[i].buf = buffers[i].buf;
            internal_ctx->buffers[i].len = buffers[i].len;
        }
        internal_ctx->buf_count = buf_count;
        internal_ctx->current_buffer = 0;
        internal_ctx->offset = 0;
        internal_ctx->ovl = ovl;
        ovl->hEvent->internal_ctx = internal_ctx;
    }

    ovl->Internal = 0;
    ovl->InternalHigh = 0;
    ovl->Offset = 0;
    ovl->OffsetHigh = 0;

    struct iovec iov[MAX_BUFFERS];
    WSABUFToIovec(buffers, buf_count, iov);

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = buf_count;

    ssize_t sent = sendmsg(sockfd, &msg, MSG_DONTWAIT);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *bytes_sent = 0;
            internal_ctx->error_code = WSA_IO_PENDING;
            errno = WSA_IO_PENDING;
            return SOCKET_ERROR;
        }
        internal_ctx->error_code = MapErrnoToWSAError(errno);
        ovl->hEvent->internal_ctx = NULL;
        free(internal_ctx);
        return SOCKET_ERROR;
    }

    *bytes_sent = sent;
    ovl->InternalHigh = sent;
    internal_ctx->bytes_transferred = sent;
    internal_ctx->complete = 1;

    uint64_t val = 1;
    write(ovl->hEvent->eventfd, &val, sizeof(val));

    return 0;
}

// Simulate WSARecv
int WSARecv(int sockfd, WSABUF *buffers, DWORD buf_count, DWORD *bytes_received, DWORD *flags, OVERLAPPED *ovl) {
    InternalContext *internal_ctx = NULL;

    if (!buffers || buf_count <= 0 || buf_count > MAX_BUFFERS || !bytes_received || !ovl || !ovl->hEvent) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    // Check if OVERLAPPED is already in use for a pending operation
    if (ovl->hEvent->internal_ctx && !((InternalContext *)ovl->hEvent->internal_ctx)->complete) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    if (ovl->hEvent->internal_ctx) {
        internal_ctx = (InternalContext *)ovl->hEvent->internal_ctx;
        // Reset fields for reuse
        internal_ctx->bytes_transferred = 0;
        internal_ctx->complete = 0;
        internal_ctx->error_code = 0;
        internal_ctx->current_buffer = 0;
        internal_ctx->offset = 0;
        internal_ctx->sock = sockfd;
        internal_ctx->op_type = OP_RECEIVE;
        for (DWORD i = 0; i < buf_count; i++) {
            internal_ctx->buffers[i].buf = buffers[i].buf;
            internal_ctx->buffers[i].len = buffers[i].len;
        }
        internal_ctx->buf_count = buf_count;
        internal_ctx->ovl = ovl;
    } else {
        internal_ctx = (InternalContext *)calloc(1, sizeof(InternalContext));
        if (!internal_ctx) {
            errno = WSAEINVAL;
            return SOCKET_ERROR;
        }
        internal_ctx->sock = sockfd;
        internal_ctx->op_type = OP_RECEIVE;
        for (DWORD i = 0; i < buf_count; i++) {
            internal_ctx->buffers[i].buf = buffers[i].buf;
            internal_ctx->buffers[i].len = buffers[i].len;
        }
        internal_ctx->buf_count = buf_count;
        internal_ctx->current_buffer = 0;
        internal_ctx->offset = 0;
        internal_ctx->ovl = ovl;
        ovl->hEvent->internal_ctx = internal_ctx;
    }

    ovl->Internal = 0;
    ovl->InternalHigh = 0;
    ovl->Offset = 0;
    ovl->OffsetHigh = 0;

    struct iovec iov[MAX_BUFFERS];
    WSABUFToIovec(buffers, buf_count, iov);

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = buf_count;

    ssize_t received = recvmsg(sockfd, &msg, MSG_DONTWAIT);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *bytes_received = 0;
            internal_ctx->error_code = WSA_IO_PENDING;
            errno = WSA_IO_PENDING;
            return SOCKET_ERROR;
        }
        internal_ctx->error_code = MapErrnoToWSAError(errno);
        ovl->hEvent->internal_ctx = NULL;
        free(internal_ctx);
        return SOCKET_ERROR;
    }

    *bytes_received = received;
    ovl->InternalHigh = received;
    internal_ctx->bytes_transferred = received;
    internal_ctx->complete = 1;
    *flags = 0;

    uint64_t val = 1;
    write(ovl->hEvent->eventfd, &val, sizeof(val));

    return 0;
}

// Simulate GetOverlappedResult
int GetOverlappedResult(int sockfd, OVERLAPPED *ovl, DWORD *bytes_transferred, int wait) {
    if (!ovl || !bytes_transferred || !ovl->hEvent) {
        errno = WSAEINVAL;
        return 0; // FALSE
    }

    InternalContext *internal_ctx = (InternalContext *)ovl->hEvent->internal_ctx;
    *bytes_transferred = internal_ctx->bytes_transferred;

    if (internal_ctx->complete) {
        if (internal_ctx->error_code) {
            errno = internal_ctx->error_code;
            return 0; // FALSE
        }
        return 1; // TRUE
    }

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = (internal_ctx->op_type == OP_RECEIVE) ? POLLIN : POLLOUT;

    int timeout = wait ? -1 : 0;
    int ret = poll(&pfd, 1, timeout);
    if (ret < 0) {
        internal_ctx->error_code = MapErrnoToWSAError(errno);
        errno = internal_ctx->error_code;
        return 0; // FALSE
    }
    if (ret == 0) {
        internal_ctx->error_code = WSAEWOULDBLOCK;
        errno = internal_ctx->error_code;
        return 0; // FALSE
    }

    struct iovec iov[MAX_BUFFERS];
    int iov_count = 0;
    for (DWORD i = internal_ctx->current_buffer; i < internal_ctx->buf_count; i++) {
        if (internal_ctx->offset > 0 && i == internal_ctx->current_buffer) {
            iov[iov_count].iov_base = internal_ctx->buffers[i].buf + internal_ctx->offset;
            iov[iov_count].iov_len = internal_ctx->buffers[i].len - internal_ctx->offset;
        } else {
            iov[iov_count].iov_base = internal_ctx->buffers[i].buf;
            iov[iov_count].iov_len = internal_ctx->buffers[i].len;
        }
        iov_count++;
    }

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = iov_count;

    ssize_t transferred = (internal_ctx->op_type == OP_RECEIVE) ? recvmsg(sockfd, &msg, MSG_DONTWAIT) : sendmsg(sockfd, &msg, MSG_DONTWAIT);
    if (transferred < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            internal_ctx->error_code = WSAEWOULDBLOCK;
            errno = internal_ctx->error_code;
            return 0; // FALSE
        }
        internal_ctx->error_code = MapErrnoToWSAError(errno);
        errno = internal_ctx->error_code;
        internal_ctx->complete = 1;
        return 0; // FALSE
    }

    internal_ctx->bytes_transferred += transferred;
    *bytes_transferred = internal_ctx->bytes_transferred;

    size_t transferred_so_far = transferred;
    for (DWORD i = internal_ctx->current_buffer; i < internal_ctx->buf_count && transferred_so_far > 0; i++) {
        size_t len = (i == internal_ctx->current_buffer) ? (internal_ctx->buffers[i].len - internal_ctx->offset) : internal_ctx->buffers[i].len;
        if (transferred_so_far >= len) {
            internal_ctx->current_buffer++;
            internal_ctx->offset = 0;
            transferred_so_far -= len;
        } else {
            internal_ctx->offset += transferred_so_far;
            transferred_so_far = 0;
        }
    }

    if (internal_ctx->current_buffer >= internal_ctx->buf_count || (internal_ctx->op_type == OP_RECEIVE && transferred == 0)) {
        internal_ctx->complete = 1;
        uint64_t val = 1;
        write(ovl->hEvent->eventfd, &val, sizeof(val));
    }

    return internal_ctx->complete ? 1 : 0;
}

// Simulate WaitForMultipleObjects
unsigned long WaitForMultipleEvents(unsigned long event_count, WSAEVENT *events, int wait_all, unsigned long timeout_ms) {
    if (!events || event_count <= 0 || event_count > MAX_OPERATIONS) {
        errno = WSAEINVAL;
        return WAIT_FAILED;
    }

    struct pollfd pfds[MAX_OPERATIONS];
    memset(pfds, 0, sizeof(pfds));
    int active_ops = 0;

    // Initialize pollfd array with eventfd
    for (unsigned long i = 0; i < event_count; i++) {
        InternalContext *internal_ctx = (InternalContext *)events[i]->internal_ctx;
        if (!internal_ctx || internal_ctx->complete) continue;
        pfds[active_ops].fd = events[i]->eventfd;
        pfds[active_ops].events = POLLIN;
        pfds[active_ops].revents = 0;
        active_ops++;
    }

    int completed = 0;
    while (active_ops > 0) {
        int ret = poll(pfds, active_ops, timeout_ms);
        if (ret < 0) {
            errno = MapErrnoToWSAError(errno);
            return WAIT_FAILED;
        }
        if (ret == 0) {
            errno = WSAETIMEDOUT;
            return WAIT_TIMEOUT;
        }

        // Process ready eventfds
        for (int i = 0; i < active_ops; i++) {
            if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                // Find the event and internal context
                WSAEVENT event = NULL;
                InternalContext *internal_ctx = NULL;
                for (unsigned long j = 0; j < event_count; j++) {
                    if (events[j]->eventfd == pfds[i].fd) {
                        event = events[j];
                        internal_ctx = (InternalContext *)event->internal_ctx;
                        break;
                    }
                }
                if (!event || !internal_ctx) continue;

                unsigned long bytes_transferred = 0;
                int result = GetOverlappedResult(internal_ctx->sock, internal_ctx->ovl, &bytes_transferred, 0);
                if (!result && internal_ctx->error_code && internal_ctx->error_code != WSAEWOULDBLOCK) {
                    internal_ctx->complete = 1;
                }
                if (internal_ctx->complete) {
                    completed++;
                    // Clear eventfd
                    uint64_t val;
                    read(pfds[i].fd, &val, sizeof(val));
                    // Remove from pollfd array
                    for (int j = i; j < active_ops - 1; j++) {
                        pfds[j] = pfds[j + 1];
                    }
                    active_ops--;
                    i--;
                    if (!wait_all && completed > 0) {
                        return WAIT_OBJECT_0 + completed - 1;
                    }
                }
            }
        }

        if (wait_all && completed == (int)event_count) {
            return WAIT_OBJECT_0;
        }
    }

    return WAIT_OBJECT_0 + completed - 1;
}

