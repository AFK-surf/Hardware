#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_INTERNAL 0
void *test_heap_alloc(size_t n);
#define heap_caps_malloc(n, caps) test_heap_alloc(n)
#define heap_caps_realloc(p, n, caps) realloc(p, n)
#define heap_caps_get_free_size(caps) ((size_t)200000)
