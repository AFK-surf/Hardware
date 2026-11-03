// SPDX-License-Identifier: Apache-2.0
// Host test for components/lan_script: the runtime with a fake gateway, and selftest.js.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lan_script.h"

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int64_t wall(void *ctx) { return (int64_t)time(NULL) * 1000; }
static int64_t mono(void *ctx) { return now_ms(); }
static void nap(void *ctx, uint32_t ms) { usleep(ms * 1000); }
static const char *cancel_reason;
static const char *cancelled(void *ctx) { return cancel_reason; }

// The fake gateway: udp_request echoes its params; anything else fails.
static bool call(void *ctx, const char *method, const char *params, size_t len, char **out)
{
    if (strcmp(method, "udp_request") == 0) {
        *out = malloc(len + 32);
        snprintf(*out, len + 32, "{\"echo\":%.*s}", (int)len, params);
        return true;
    }
    *out = strdup("connect to 192.168.1.9:80 refused");
    return false;
}

static const lan_script_host_t HOST = {call, wall, mono, nap, cancelled, NULL};
static uint8_t heap[256 * 1024];
static const char *selftest_source;

static char *run(const char *source, const char *args, size_t heap_bytes, int64_t ms)
{
    lan_script_limits_t limits = {heap, heap_bytes, now_ms() + ms, 4096, 256};
    char *out = lan_script_run(&HOST, &limits, source, strlen(source), args, args ? strlen(args) : 0);
    assert(out);
    return out;
}

static void expect(const char *name, const char *source, const char *want)
{
    char *out = run(source, NULL, sizeof(heap), 2000);
    if (!strstr(out, want)) {
        fprintf(stderr, "FAIL %s: %s\n  want: %s\n", name, out, want);
        exit(1);
    }
    printf("ok %s\n", name);
    free(out);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    assert(fread(buf, 1, n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    char *selftest = read_file(argc > 1 ? argv[1] : "selftest.js");
    char *out = run(selftest, "{\"n\":1}", sizeof(heap), 5000);
    if (strcmp(out, "{\"result\":\"ok\",\"logs\":[\"selftest {\\\"done\\\":true}\"]}") != 0) {
        fprintf(stderr, "FAIL selftest: %s\n", out);
        return 1;
    }
    printf("ok selftest\n");
    free(out);
    selftest_source = selftest;

    expect("result", "return {a: [1, 'x'], b: null};", "{\"result\":{\"a\":[1,\"x\"],\"b\":null},\"logs\":[]}");
    expect("undefined", "var x = 1;", "{\"result\":null,");
    expect("lan.udp", "return lan.udp({host: '192.168.1.5', port: 7000}).echo.port;", "{\"result\":7000,");
    expect("lan error", "return lan.http({url: 'http://192.168.1.9/'});", "refused");
    expect("lan error caught", "try { lan.http({}); } catch (e) { return e.message; }", "{\"result\":\"connect to 192.168.1.9:80 refused\"");
    expect("throw", "throw new Error('nope');", "{\"error\":\"Error: nope");
    expect("syntax", "return (;", "{\"error\":\"SyntaxError");
    expect("line", "var a = 1;\nnull.x;", "script:2");
    expect("timeout", "for (;;) {}", "{\"error\":\"the script timed out\"");
    expect("clocks", "var t = performance.now(); sleep(20); return [typeof Date.now(), performance.now() - t >= 20];",
           "{\"result\":[\"number\",true],");
    expect("timeout in sleep", "sleep(100000); return 1;", "{\"error\":\"the script timed out\"");
    expect("memory", "var a = []; for (;;) a.push('xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' + a.length);", "{\"error\":");
    expect("result size", "var s = ''; for (var i = 0; i < 5000; i++) s += 'x'; return s;", "at most 4096 are returned");
    expect("logs bound", "for (var i = 0; i < 100; i++) console.log('line ' + i); return 1;", "\"logs_truncated\":true}");
    expect("strict", "undeclared = 1;", "{\"error\":");
    cancel_reason = "access turned off";
    expect("cancelled", "for (;;) {}", "{\"error\":\"access turned off\"");
    cancel_reason = NULL;

    char *small = run("return 1;", NULL, 1024, 1000);
    assert(strstr(small, "\"error\":"));
    printf("ok tiny heap: %s\n", small);
    free(small);
    // The smallest heap the selftest runs in, for the boards' Kconfig defaults.
    for (size_t kb = 16; kb <= 256; kb += 4) {
        char *o = run(selftest_source, "{\"n\":1}", kb * 1024, 5000);
        bool ok = strstr(o, "\"result\":\"ok\"") != NULL;
        free(o);
        if (ok) {
            printf("selftest runs in %zu KiB\n", kb);
            break;
        }
    }
    puts("lan_script_test: all passed");
    return 0;
}
