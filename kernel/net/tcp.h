/* TCP: nehany aktiv kapcsolat, stop-and-wait kuldes (egy szegmens uton, ujrakuldes),
 * sorrendben erkezo adat fogadasa 16 KiB-os pufferrel, nincs kiszolgalo oldal (listen). */
#pragma once
#include "types.h"

#define TCP_SOCKS 4
#define TCP_RXBUF 16384
#define TCP_MSS   1024

void tcp_init(void);
void tcp_tick(void);                                           /* 10 ms-enkent */
void tcp_rx(u32 src, u32 dst, const u8 *seg, usize len);

int   tcp_connect(u32 ip, u16 port, u32 timeout_ms);           /* -> sock id vagy hiba */
isize tcp_send(int sock, const void *buf, usize n);            /* blokkol az ACK-ig */
isize tcp_recv(int sock, void *buf, usize n, u32 timeout_ms);  /* 0 = zarva, E_TIMEOUT */
int   tcp_close(int sock);
bool  tcp_connected(int sock);
usize tcp_available(int sock);
const char *tcp_state_name(int sock);
