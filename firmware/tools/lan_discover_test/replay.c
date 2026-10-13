// SPDX-License-Identifier: Apache-2.0
// Replay captured answers through lan_discover_core.c and print the result JSON.
// Files are named <mdns|ssdp|yl>_<n>_<source ip>.bin (one datagram each), as tools/capture.py writes.
// Run: ./replay.sh <self ip> <netmask> <capture files...>

#include "../../main/lan_discover_core.c"

#include <arpa/inet.h>
#include <libgen.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s self_ip netmask files...\n", argv[0]);
        return 2;
    }
    ld_ctx_t *ctx = ld_create(inet_addr(argv[1]), inet_addr(argv[2]));
    for (int i = 3; i < argc; ++i) {
        char name[256];
        snprintf(name, sizeof(name), "%s", basename(argv[i]));
        char kind[8], src[32];
        int n;
        if (sscanf(name, "%7[a-z]_%d_%31[0-9.].bin", kind, &n, src) != 3) {
            fprintf(stderr, "skip %s\n", name);
            continue;
        }
        FILE *f = fopen(argv[i], "rb");
        static uint8_t buf[9000];
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        if (strcmp(kind, "mdns") == 0) {
            ld_add_mdns(ctx, inet_addr(src), buf, len);
        } else {
            ld_add_ssdp(ctx, inet_addr(src), (const char *)buf, len, strcmp(kind, "yl") == 0 ? "wifi_bulb" : NULL);
        }
    }
    char *json = ld_result_json(ctx);
    puts(json);
    free(json);
    ld_free(ctx);
    return 0;
}
