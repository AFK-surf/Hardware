#pragma once
#include "../../lan_http_test/stubs/esp_heap_caps.h"
#undef heap_caps_realloc
void *ld_test_realloc(void *p, size_t n);
#define heap_caps_realloc(p, n, caps) ld_test_realloc(p, n)

#undef heap_caps_malloc
void *ld_test_malloc(size_t n);
#define heap_caps_malloc(n, caps) ld_test_malloc(n)
