#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_realloc(p, n, caps) realloc(p, n)
