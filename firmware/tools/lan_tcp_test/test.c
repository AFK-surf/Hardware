// SPDX-License-Identifier: Apache-2.0
// Host test for main/lan_tcp.c: builds it on the Mac with BSD sockets. Every connection the firmware
// opens goes to a local listener instead (stubs/lwip/sockets.h), which plays the LAN host.
// Run: ./run.sh

#include "../../main/lan_dest.c"
#include "../../main/lan_tcp.c"

#undef connect

#include <pthread.h>

static int g_checks, g_failed;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        g_checks++;                                              \
        if (!(cond)) {                                           \
            g_failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

bool wifi_ip_info(uint32_t *ip, uint32_t *netmask)
{
    *ip = inet_addr("192.168.1.17");
    *netmask = inet_addr("255.255.255.0");
    return true;
}

// Where connections really go: the listener's port, or a closed port.
static uint16_t g_port;

int test_connect(int s, const struct sockaddr *to, socklen_t len)
{
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(g_port)};
    local.sin_addr.s_addr = inet_addr("127.0.0.1");
    return connect(s, (struct sockaddr *)&local, sizeof(local));
}

// The LAN host: what it does with one connection.
typedef enum { ECHO_UNTIL_EOF, SEND_AND_HOLD, SEND_BIG } peer_mode_t;
static peer_mode_t g_mode;
static int g_listener;

static void *peer(void *arg)
{
    int c = accept(g_listener, NULL, NULL);
    if (c < 0) return NULL;
    char buf[4096];
    if (g_mode == ECHO_UNTIL_EOF) {
        ssize_t n;
        char all[8192];
        size_t got = 0;
        while ((n = recv(c, buf, sizeof(buf), 0)) > 0 && got + n <= sizeof(all)) {
            memcpy(all + got, buf, n);
            got += n;
        }
        send(c, "echo:", 5, 0);
        send(c, all, got, 0);
    } else if (g_mode == SEND_AND_HOLD) {
        send(c, "hello", 5, 0);
        sleep(3);
    } else {
        memset(buf, 'x', sizeof(buf));
        for (int i = 0; i < 4; i++) send(c, buf, sizeof(buf), 0);
        sleep(1);
    }
    close(c);
    return NULL;
}

static cJSON *run(const char *params_json, peer_mode_t mode, bool listen_on)
{
    g_mode = mode;
    pthread_t t;
    g_listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(g_listener, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(g_listener, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    if (listen_on) {
        listen(g_listener, 1);
        pthread_create(&t, NULL, peer, NULL);
    } else {
        close(g_listener);  // a port nobody listens on
        g_listener = -1;
    }
    cJSON *params = cJSON_Parse(params_json);
    char *frame = lan_tcp_run_request("t", params, NULL);
    cJSON_Delete(params);
    if (listen_on) pthread_join(t, NULL);
    if (g_listener >= 0) close(g_listener);
    cJSON *reply = cJSON_Parse(frame);
    free(frame);
    return reply;
}

static const char *result_str(cJSON *reply, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply, "result"), key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static bool result_true(cJSON *reply, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply, "result"), key));
}

static const char *error_of(cJSON *reply)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(reply, "error");
    return cJSON_IsString(e) ? e->valuestring : "";
}

int main(void)
{
    // "hi" out, the write side shut: the host answers and closes.
    cJSON *r = run("{\"host\":\"192.168.1.40\",\"port\":631,\"send_b64\":\"aGk=\",\"close_write\":true}", ECHO_UNTIL_EOF, true);
    CHECK(strcmp(result_str(r, "data_b64"), "ZWNobzpoaQ==") == 0, "echo: %s", cJSON_PrintUnformatted(r));
    CHECK(result_true(r, "closed") && !result_true(r, "truncated") && !result_true(r, "timed_out"), "echo flags");
    cJSON_Delete(r);

    // A host that says something and keeps the connection: the idle wait ends the read.
    r = run("{\"host\":\"192.168.1.40\",\"port\":9100,\"idle_ms\":300}", SEND_AND_HOLD, true);
    CHECK(strcmp(result_str(r, "data_b64"), "aGVsbG8=") == 0, "idle: %s", cJSON_PrintUnformatted(r));
    CHECK(!result_true(r, "closed") && !result_true(r, "timed_out"), "idle flags");
    cJSON_Delete(r);

    // The whole timeout runs out first.
    r = run("{\"host\":\"192.168.1.40\",\"port\":9100,\"idle_ms\":0,\"timeout_ms\":300}", SEND_AND_HOLD, true);
    CHECK(result_true(r, "timed_out"), "timeout: %s", cJSON_PrintUnformatted(r));
    cJSON_Delete(r);

    // More than max_bytes.
    r = run("{\"host\":\"192.168.1.40\",\"port\":9100,\"max_bytes\":100}", SEND_BIG, true);
    CHECK(result_true(r, "truncated") && strlen(result_str(r, "data_b64")) == B64_ENCODED_LEN(100), "truncated: %s",
          cJSON_PrintUnformatted(r));
    cJSON_Delete(r);

    // Nothing listening; a public address; bad params.
    r = run("{\"host\":\"192.168.1.40\",\"port\":9100}", SEND_BIG, false);
    CHECK(strstr(error_of(r), "refused") != NULL, "refused: %s", error_of(r));
    cJSON_Delete(r);
    r = run("{\"host\":\"8.8.8.8\",\"port\":53}", SEND_BIG, false);
    CHECK(error_of(r)[0] != 0, "public address accepted");
    cJSON_Delete(r);
    r = run("{\"host\":\"192.168.1.40\",\"port\":9100,\"send_b64\":\"abc\"}", SEND_BIG, false);
    CHECK(strstr(error_of(r), "base64") != NULL, "bad base64: %s", error_of(r));
    cJSON_Delete(r);

    printf("lan_tcp_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
