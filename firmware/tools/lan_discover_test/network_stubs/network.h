// SPDX-License-Identifier: Apache-2.0
#pragma once
// Deterministic stand-ins for the discovery runner's clock, ARP table and socket system calls.
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARP_TABLE_SIZE 32
#define ERR_OK 0
#define lwip_htonl htonl
#define lwip_ntohl ntohl
#define pdMS_TO_TICKS(ms) (ms)
typedef int err_t;
typedef struct { uint32_t addr; } ip4_addr_t;
struct eth_addr { uint8_t addr[6]; };
struct netif { int unused; };
struct tcpip_api_call_data { int unused; };
typedef struct netif esp_netif_t;

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
void *esp_netif_get_netif_impl(esp_netif_t *netif);
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *), struct tcpip_api_call_data *data);
err_t etharp_request(struct netif *netif, const ip4_addr_t *ip);
int etharp_get_entry(size_t i, ip4_addr_t **ip, struct netif **netif, struct eth_addr **mac);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned ticks);

int test_socket(int domain, int type, int protocol);
int test_close(int s);
int test_bind(int s, const struct sockaddr *addr, socklen_t len);
int test_setsockopt(int s, int level, int opt, const void *value, socklen_t len);
int test_select(int n, fd_set *read, fd_set *write, fd_set *error, struct timeval *wait);
ssize_t test_sendto(int s, const void *data, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t test_recvfrom(int s, void *data, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
#define socket test_socket
#define close test_close
#define bind test_bind
#define setsockopt test_setsockopt
#define select test_select
#define sendto test_sendto
#define recvfrom test_recvfrom
