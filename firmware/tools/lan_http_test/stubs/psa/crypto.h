#pragma once
// The PSA hash calls lan_http.c uses, on macOS CommonCrypto.
#include <CommonCrypto/CommonDigest.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int psa_status_t;
typedef int psa_algorithm_t;
#define PSA_SUCCESS 0
#define PSA_ERROR_BAD_STATE (-137)
#define PSA_ALG_SHA_256 0x02000009

typedef struct {
    int active;
    CC_SHA256_CTX ctx;
} psa_hash_operation_t;

static inline psa_hash_operation_t psa_hash_operation_init(void)
{
    psa_hash_operation_t op;
    memset(&op, 0, sizeof(op));
    return op;
}

static inline psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    (void)alg;
    CC_SHA256_Init(&op->ctx);
    op->active = 1;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t n)
{
    if (!op->active) {
        return PSA_ERROR_BAD_STATE;
    }
    CC_SHA256_Update(&op->ctx, in, (CC_LONG)n);
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t size, size_t *len)
{
    if (!op->active || size < 32) {
        return PSA_ERROR_BAD_STATE;
    }
    CC_SHA256_Final(out, &op->ctx);
    op->active = 0;
    *len = 32;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    op->active = 0;
    return PSA_SUCCESS;
}
