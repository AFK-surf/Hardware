// SPDX-License-Identifier: Apache-2.0
#include "connector_json.h"
#include "esp_heap_caps.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_alloc_failed_hook_t failed_hook;
static int checks, oom_calls, allocations, live, fail_at;
static bool fail_other_task, fail_interrupt;
static _Thread_local bool in_isr;
static const char *last_oom;

#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    failed_hook = callback;
    return 0;
}

bool xPortInIsrContext(void) { return in_isr; }

// Each failure logged by the hook asks for the free and largest block of the failed caps.
static int largest_queries;
static uint32_t largest_caps;
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 45904; }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    ++largest_queries;
    largest_caps = caps;
    return 7168;
}

static void *other_task(void *arg)
{
    (void)arg;
    failed_hook(64, 0, "malloc");
    return NULL;
}

static void *test_malloc(size_t n)
{
    if (fail_other_task) {
        fail_other_task = false;
        pthread_t th;
        CHECK(pthread_create(&th, NULL, other_task, NULL) == 0);
        CHECK(pthread_join(th, NULL) == 0);
    }
    if (fail_interrupt) {
        fail_interrupt = false;
        in_isr = true;
        failed_hook(64, 0, "malloc");
        in_isr = false;
    }
    if (++allocations == fail_at) {
        failed_hook(n, 0, "malloc");
        return NULL;
    }
    void *p = malloc(n);
    if (p) ++live;
    return p;
}

static void test_free(void *p)
{
    if (p) --live;
    free(p);
}

static void on_oom(const char *json)
{
    ++oom_calls;
    last_oom = json;
}

static void ignored(const char *json)
{
    int before = oom_calls;
    CHECK(connector_json_parse(json, on_oom) == NULL);
    CHECK(oom_calls == before); // The Connector's error/session-close callback must not run.
    CHECK(live == 0);
}

int main(void)
{
    connector_json_init();
    CHECK(failed_hook != NULL);
    // Firmware keeps cJSON's default allocator. Only this host allocator simulates
    // IDF's notification when a malloc really returns NULL.
    cJSON_Hooks hooks = {.malloc_fn = test_malloc, .free_fn = test_free};
    cJSON_InitHooks(&hooks);
    ignored("{\"id\":\"r1\",\"type\":");
    ignored("{");
    ignored("not JSON");
    ignored("");
    const char *valid = "{\"id\":\"r1\",\"type\":\"request\",\"params\":{\"value\":[1,true,null]}}";
    allocations = 0;
    cJSON *root = connector_json_parse(valid, on_oom);
    CHECK(root != NULL && oom_calls == 0);
    int total = allocations;
    cJSON_Delete(root);
    CHECK(live == 0);
    // Fail every allocation, including nested nodes and string copies.
    for (int i = 1; i <= total; ++i) {
        allocations = 0;
        fail_at = i;
        int before = oom_calls;
        CHECK(connector_json_parse(valid, on_oom) == NULL);
        CHECK(oom_calls == before + 1 && last_oom == valid);
        CHECK(live == 0);
        fail_at = 0;
        ignored("{\"id\":\"bad\",}"); // Failure state cannot leak into the next frame.
    }
    allocations = 0;
    fail_at = 1;
    int before = oom_calls;
    const char *no_id = "{\"type\":\"heartbeat\"}";
    CHECK(connector_json_parse(no_id, on_oom) == NULL);
    CHECK(oom_calls == before + 1 && last_oom == no_id);
    fail_at = 0;
    fail_other_task = true;
    ignored("{\"id\":\"bad\",}");
    CHECK(!fail_other_task);
    fail_interrupt = true;
    ignored("{\"id\":\"bad\",}");
    CHECK(!fail_interrupt);
    failed_hook(64, 0, "malloc"); // An earlier failure outside parsing is unrelated too.
    ignored("{");
    before = oom_calls;
    root = connector_json_parse(valid, on_oom);
    CHECK(root != NULL && oom_calls == before);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(root, "id")->valuestring, "r1") == 0);
    cJSON_Delete(root);
    CHECK(live == 0);
    // Failed allocations are logged only while this task asked for it, never from an interrupt.
    CHECK(largest_queries == 0);
    comma_log_failed_allocs(true);
    failed_hook(12289, 0x1804, "heap_caps_realloc");
    CHECK(largest_queries == 1 && largest_caps == 0x1804);
    fail_other_task = true;
    ignored("{\"id\":\"bad\",}");
    CHECK(largest_queries == 1);
    in_isr = true;
    failed_hook(64, 0, "malloc");
    in_isr = false;
    CHECK(largest_queries == 1);
    comma_log_failed_allocs(false);
    failed_hook(64, 0, "malloc");
    CHECK(largest_queries == 1);
    cJSON_InitHooks(NULL);
    printf("%d checks, 0 failed\n", checks);
    return 0;
}
