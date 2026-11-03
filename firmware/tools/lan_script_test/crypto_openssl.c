// SPDX-License-Identifier: Apache-2.0
// lan_script_crypto.h on OpenSSL, for the host test only (the firmware uses lan_script_crypto_psa.c).
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "lan_script_crypto.h"

static const EVP_MD *md(lan_hash_t alg) { return alg == LAN_HASH_MD5 ? EVP_md5() : alg == LAN_HASH_SHA1 ? EVP_sha1() : EVP_sha256(); }

size_t lan_crypto_hash_len(lan_hash_t alg) { return alg == LAN_HASH_MD5 ? 16 : alg == LAN_HASH_SHA1 ? 20 : 32; }

int lan_crypto_hash(lan_hash_t alg, const uint8_t *in, size_t len, uint8_t *out)
{
    return EVP_Digest(in, len, out, NULL, md(alg), NULL) == 1 ? 0 : -1;
}

int lan_crypto_hmac(lan_hash_t alg, const uint8_t *key, size_t key_len, const uint8_t *in, size_t len, uint8_t *out)
{
    unsigned n;
    return HMAC(md(alg), key, (int)key_len, in, len, out, &n) ? 0 : -1;
}

static const EVP_CIPHER *aes(bool cbc, bool gcm, size_t key_len)
{
    if (gcm) return key_len == 16 ? EVP_aes_128_gcm() : key_len == 24 ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
    if (cbc) return key_len == 16 ? EVP_aes_128_cbc() : key_len == 24 ? EVP_aes_192_cbc() : EVP_aes_256_cbc();
    return key_len == 16 ? EVP_aes_128_ecb() : key_len == 24 ? EVP_aes_192_ecb() : EVP_aes_256_ecb();
}

int lan_crypto_aes_block(bool cbc, bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *iv,
                         const uint8_t *in, size_t len, uint8_t *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0, m = 0;
    int ok = EVP_CipherInit_ex(c, aes(cbc, false, key_len), NULL, key, iv, encrypt) == 1 &&
             EVP_CIPHER_CTX_set_padding(c, 0) == 1 && EVP_CipherUpdate(c, out, &n, in, (int)len) == 1 &&
             EVP_CipherFinal_ex(c, out + n, &m) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok && (size_t)(n + m) == len ? 0 : -1;
}

int lan_crypto_aes_gcm(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                       const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                       uint8_t tag[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0, m = 0, a = 0;
    int ok = EVP_CipherInit_ex(c, aes(false, true, key_len), NULL, NULL, NULL, encrypt) == 1 &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)iv_len, NULL) == 1 &&
             EVP_CipherInit_ex(c, NULL, NULL, key, iv, encrypt) == 1 &&
             (!aad_len || EVP_CipherUpdate(c, NULL, &a, aad, (int)aad_len) == 1) &&
             EVP_CipherUpdate(c, out, &n, in, (int)len) == 1 &&
             (encrypt || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1) &&
             EVP_CipherFinal_ex(c, out + n, &m) == 1 &&
             (!encrypt || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
    EVP_CIPHER_CTX_free(c);
    return ok && (size_t)(n + m) == len ? 0 : -1;
}

int lan_crypto_random(uint8_t *out, size_t len) { return RAND_bytes(out, (int)len) == 1 ? 0 : -1; }
