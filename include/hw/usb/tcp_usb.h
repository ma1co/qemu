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

#ifndef HW_TCP_USB
#define HW_TCP_USB

typedef enum TcpUsbFlagEnum {
    tcp_usb_setup = 1 << 0,
    tcp_usb_reset = 1 << 1,
} TcpUsbFlagEnum;

typedef enum TcpUsbStateEnum {
    tcp_usb_read,
    tcp_usb_write,
} TcpUsbStateEnum;

typedef struct TcpUsbHeader {
    uint8_t flags;
    uint8_t ep;
    uint8_t pad[2];
    int32_t length;
} TcpUsbHeader;
QEMU_BUILD_BUG_ON(sizeof(TcpUsbHeader) != 8);

typedef int (*TcpUsbCallback)(void *arg, const TcpUsbHeader *header, char *buffer);

typedef struct TcpUsbState {
    int server_socket;
    int client_socket;

    TcpUsbStateEnum state;
    TcpUsbHeader header;
    char *buffer;
    size_t count;

    TcpUsbCallback callback;
    void *callback_arg;
} TcpUsbState;

void tcp_usb_init(TcpUsbState *s, TcpUsbCallback callback, void *arg);
void tcp_usb_cleanup(TcpUsbState *s);
int tcp_usb_serve(TcpUsbState *s, int port);

#endif
