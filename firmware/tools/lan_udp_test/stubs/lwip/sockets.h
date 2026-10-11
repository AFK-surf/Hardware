#pragma once
// BSD sockets stand in for lwIP's. sendto/recvfrom go through test.c, which sends every datagram to
// a local peer (whatever LAN address the firmware picked) and makes answers look like they came from
// the addresses the test chose.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
ssize_t test_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t test_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
#define sendto test_sendto
#define recvfrom test_recvfrom
