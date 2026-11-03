// SPDX-License-Identifier: Apache-2.0
#pragma once
// BSD sockets stand in for lwIP's. connect goes through test.c, which sends every connection to a
// local listener whatever LAN address the firmware picked.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
int test_connect(int s, const struct sockaddr *to, socklen_t len);
#define connect test_connect
