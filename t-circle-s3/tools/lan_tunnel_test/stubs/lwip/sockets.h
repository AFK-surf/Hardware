#pragma once
// BSD sockets stand in for lwIP's. connect() goes through test.c, which sends every LAN address the
// firmware dials to a local server chosen by the test (and records what was asked for).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
int test_connect(int fd, const struct sockaddr *to, socklen_t len);
#define connect test_connect
