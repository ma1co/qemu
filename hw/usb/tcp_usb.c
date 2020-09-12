/*
 * USB over TCP driver.
 *
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/usb.h"
#include "hw/usb/tcp_usb.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "sysemu/sysemu.h"

void tcp_usb_init(TcpUsbState *s, TcpUsbCallback callback, void *arg)
{
    s->server_socket = -1;
    s->client_socket = -1;

    s->callback = callback;
    s->callback_arg = arg;

    s->state = tcp_usb_read;
    s->count = 0;
    s->buffer = NULL;
}

static void tcp_usb_client_cleanup(TcpUsbState *s)
{
    if (s->client_socket >= 0) {
        closesocket(s->client_socket);
        qemu_set_fd_handler(s->client_socket, NULL, NULL, NULL);
        s->client_socket = -1;
    }

    if (s->buffer) {
        free(s->buffer);
        s->buffer = NULL;
    }
}

void tcp_usb_cleanup(TcpUsbState *s)
{
    tcp_usb_client_cleanup(s);

    if (s->server_socket >= 0) {
        closesocket(s->server_socket);
        qemu_set_fd_handler(s->server_socket, NULL, NULL, NULL);
        s->server_socket = -1;
    }

    s->callback = NULL;
    s->callback_arg = NULL;
}

static void tcp_usb_client_closed(TcpUsbState *s)
{
    qemu_set_fd_handler(s->client_socket, NULL, NULL, NULL);
    s->client_socket = -1;
    tcp_usb_client_cleanup(s);
}

static void tcp_usb_callback(TcpUsbState *s, int can_read, int can_write)
{
    int ret;

    switch (s->state) {
        case tcp_usb_read:
            if (!can_read) {
                return;
            }

            if (s->count < sizeof(s->header)) {
                ret = recv(s->client_socket,
                        ((char *) &s->header) + s->count,
                        sizeof(s->header) - s->count, 0);
                if (ret == 0) {
                    tcp_usb_client_closed(s);
                    return;
                } else if (ret < 0 && errno == ECONNRESET) {
                    tcp_usb_client_cleanup(s);
                    return;
                } else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                    return;
                } else if (ret < 0) {
                    fprintf(stderr, "%s: read error %d.\n", __func__, errno);
                    return;
                }

                s->count += ret;
                if (s->count < sizeof(s->header)) {
                    return;
                }

                if (s->header.length > 0) {
                    s->buffer = malloc(s->header.length);
                }
            }

            if (!(s->header.ep & USB_DIR_IN) && s->header.length > 0) {
                ret = recv(s->client_socket,
                        s->buffer + (s->count - sizeof(s->header)),
                        s->header.length - (s->count - sizeof(s->header)), 0);
                if (ret == 0) {
                    tcp_usb_client_closed(s);
                    return;
                } else if (ret < 0 && errno == ECONNRESET) {
                    tcp_usb_client_cleanup(s);
                    return;
                } else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                    return;
                } else if (ret < 0) {
                    fprintf(stderr, "%s: read error %d.\n", __func__, errno);
                    return;
                }

                s->count += ret;
                if (s->count < sizeof(s->header) + s->header.length) {
                    return;
                }
            }

            if (s->callback) {
                s->header.length = s->callback(s->callback_arg, &s->header, s->buffer);
            }

            s->state = tcp_usb_write;
            s->count = 0;

            // fall through
        case tcp_usb_write:
            if (!can_write) {
                return;
            }

            if (s->count < sizeof(s->header)) {
                ret = send(s->client_socket,
                        ((char *) &s->header) + s->count,
                        sizeof(s->header) - s->count, 0);
                if (ret == 0) {
                    tcp_usb_client_closed(s);
                    return;
                } else if (ret < 0 && errno == ECONNRESET) {
                    tcp_usb_client_cleanup(s);
                    return;
                } else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                    return;
                } else if (ret < 0) {
                    fprintf(stderr, "%s: write error %d.\n", __func__, errno);
                    return;
                }

                s->count += ret;
                if (s->count < sizeof(s->header)) {
                    return;
                }
            }

            if ((s->header.ep & USB_DIR_IN) && s->header.length > 0) {
                ret = send(s->client_socket,
                        s->buffer + (s->count - sizeof(s->header)),
                        s->header.length - (s->count - sizeof(s->header)), 0);
                if (ret == 0) {
                    tcp_usb_client_closed(s);
                    return;
                } else if (ret < 0 && errno == ECONNRESET) {
                    tcp_usb_client_cleanup(s);
                    return;
                } else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                    return;
                } else if (ret < 0) {
                    fprintf(stderr, "%s: write error %d.\n", __func__, errno);
                    return;
                }

                s->count += ret;
                if (s->count < sizeof(s->header) + s->header.length) {
                    return;
                }
            }

            if (s->buffer) {
                free(s->buffer);
                s->buffer = NULL;
            }

            s->state = tcp_usb_read;
            s->count = 0;
    }
}

static void tcp_usb_read_callback(void *arg)
{
    TcpUsbState *s = arg;
    tcp_usb_callback(s, 1, 0);
}

static void tcp_usb_write_callback(void *arg)
{
    TcpUsbState *s = arg;
    tcp_usb_callback(s, 0, 1);
}

static void tcp_usb_accept(void *arg)
{
    TcpUsbState *s = arg;

    int ret = qemu_accept(s->server_socket, NULL, NULL);
    if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        return;
    } else if (ret < 0) {
        fprintf(stderr, "%s: accept error %d.\n", __func__, errno);
        return;
    }

    if (s->client_socket >= 0) {
        closesocket(ret);
        return;
    }

    s->client_socket = ret;
    s->state = tcp_usb_read;
    s->count = 0;

    qemu_set_nonblock(s->client_socket);
    socket_set_nodelay(s->client_socket);
    qemu_set_fd_handler(s->client_socket, tcp_usb_read_callback, tcp_usb_write_callback, s);
}

int tcp_usb_serve(TcpUsbState *s, int port)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (s->server_socket >= 0) {
        return 0;
    }

    s->server_socket = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (s->server_socket < 0) {
        return -1;
    }

    if (bind(s->server_socket, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        return -1;
    }

    if (listen(s->server_socket, 1) < 0) {
        return -1;
    }

    qemu_set_nonblock(s->server_socket);
    qemu_set_fd_handler(s->server_socket, tcp_usb_accept, NULL, s);

    return 0;
}
