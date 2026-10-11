#pragma once
#include <arpa/inet.h>
#define inet_ntoa_r(addr, buf, len) inet_ntop(AF_INET, &(addr), (buf), (len))
