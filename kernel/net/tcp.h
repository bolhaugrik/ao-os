/* TCP: nehany aktiv kapcsolat, csuszo ablakos kuldes (32 KiB uton, go-back-N ujrakuldes),
 * sorrendben erkezo adat fogadasa 64 KiB-os pufferrel, nincs kiszolgalo oldal (listen). */
#pragma once
#include "types.h"

#define TCP_SOCKS 4
#define TCP_RXBUF 65536
#define TCP_TXBUF 32768     /* kuldo ablak: ennyi lehet uton nyugtazatlanul */
#define TCP_MSS   1460      /* Ethernet MTU: 1500 - 20 IP - 20 TCP (FRAME_MAX 1536-ba belefer) */

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
