// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

// Name lookups never reach the machine's resolver, whose answers vary (a proxy's fake-IP DNS answers
// even `.invalid` names). "localhost" is loopback; every other name is unknown.
static inline int test_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                                   struct addrinfo **res)
{
    (void)service;
    (void)hints;
    if (!node || strcmp(node, "localhost") != 0) {
        return EAI_NONAME;
    }
    struct {
        struct addrinfo info;
        struct sockaddr_in addr;
    } *r = calloc(1, sizeof(*r));
    if (!r) {
        return EAI_MEMORY;
    }
    r->addr.sin_family = AF_INET;
    r->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r->info.ai_family = AF_INET;
    r->info.ai_socktype = SOCK_STREAM;
    r->info.ai_addr = (struct sockaddr *)&r->addr;
    r->info.ai_addrlen = sizeof(r->addr);
    *res = &r->info;
    return 0;
}

static inline void test_freeaddrinfo(struct addrinfo *res)
{
    free(res);
}

#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
