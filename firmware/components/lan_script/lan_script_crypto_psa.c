// SPDX-License-Identifier: Apache-2.0
// lan_script_crypto.h on PSA Crypto (ESP-IDF 6's mbedTLS). Each call imports its key as a volatile key
// and destroys it before returning.

#include <string.h>

#include "lan_script_crypto.h"
#include "psa/crypto.h"

static psa_algorithm_t hash_alg(lan_hash_t alg)
{
    return alg == LAN_HASH_MD5 ? PSA_ALG_MD5 : alg == LAN_HASH_SHA1 ? PSA_ALG_SHA_1 : PSA_ALG_SHA_256;
}

size_t lan_crypto_hash_len(lan_hash_t alg) { return alg == LAN_HASH_MD5 ? 16 : alg == LAN_HASH_SHA1 ? 20 : 32; }

int lan_crypto_hash(lan_hash_t alg, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t out_len;
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    return psa_hash_compute(hash_alg(alg), in, len, out, lan_crypto_hash_len(alg), &out_len) == PSA_SUCCESS ? 0 : -1;
}

static int import(psa_key_type_t type, psa_key_usage_t usage, psa_algorithm_t alg, const uint8_t *key, size_t key_len,
                  psa_key_id_t *id)
{
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, type);
    psa_set_key_usage_flags(&attrs, usage);
    psa_set_key_algorithm(&attrs, alg);
    psa_set_key_bits(&attrs, key_len * 8);
    psa_status_t status = psa_import_key(&attrs, key, key_len, id);
    psa_reset_key_attributes(&attrs);
    return status == PSA_SUCCESS ? 0 : -1;
}

int lan_crypto_hmac(lan_hash_t alg, const uint8_t *key, size_t key_len, const uint8_t *in, size_t len, uint8_t *out)
{
    psa_key_id_t id;
    const psa_algorithm_t mac = PSA_ALG_HMAC(hash_alg(alg));
    // PSA refuses an empty HMAC key; HMAC pads every key to the block size, so one zero byte is the same.
    static const uint8_t zero = 0;
    if (import(PSA_KEY_TYPE_HMAC, PSA_KEY_USAGE_SIGN_MESSAGE, mac, key_len ? key : &zero, key_len ? key_len : 1, &id))
        return -1;
    size_t out_len;
    psa_status_t status = psa_mac_compute(id, mac, in, len, out, lan_crypto_hash_len(alg), &out_len);
    psa_destroy_key(id);
    return status == PSA_SUCCESS ? 0 : -1;
}

int lan_crypto_aes_block(bool cbc, bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *iv,
                         const uint8_t *in, size_t len, uint8_t *out)
{
    const psa_algorithm_t alg = cbc ? PSA_ALG_CBC_NO_PADDING : PSA_ALG_ECB_NO_PADDING;
    psa_key_id_t id;
    if (import(PSA_KEY_TYPE_AES, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT, alg, key, key_len, &id))
        return -1;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t done = 0, last = 0;
    psa_status_t status = encrypt ? psa_cipher_encrypt_setup(&op, id, alg) : psa_cipher_decrypt_setup(&op, id, alg);
    if (status == PSA_SUCCESS && cbc) status = psa_cipher_set_iv(&op, iv, 16);
    if (status == PSA_SUCCESS) status = psa_cipher_update(&op, in, len, out, len, &done);
    if (status == PSA_SUCCESS) status = psa_cipher_finish(&op, out + done, len - done, &last);
    psa_cipher_abort(&op);
    psa_destroy_key(id);
    return status == PSA_SUCCESS && done + last == len ? 0 : -1;
}

int lan_crypto_aes_gcm(bool encrypt, const uint8_t *key, size_t key_len, const uint8_t *iv, size_t iv_len,
                       const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                       uint8_t tag[16])
{
    psa_key_id_t id;
    if (import(PSA_KEY_TYPE_AES, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT, PSA_ALG_GCM, key, key_len, &id))
        return -1;
    psa_aead_operation_t op = PSA_AEAD_OPERATION_INIT;
    size_t done = 0, last = 0, tag_len = 0;
    psa_status_t status = encrypt ? psa_aead_encrypt_setup(&op, id, PSA_ALG_GCM) : psa_aead_decrypt_setup(&op, id, PSA_ALG_GCM);
    if (status == PSA_SUCCESS) status = psa_aead_set_nonce(&op, iv, iv_len);
    if (status == PSA_SUCCESS && aad_len) status = psa_aead_update_ad(&op, aad, aad_len);
    if (status == PSA_SUCCESS) status = psa_aead_update(&op, in, len, out, len, &done);
    if (status == PSA_SUCCESS)
        status = encrypt ? psa_aead_finish(&op, out + done, len - done, &last, tag, 16, &tag_len)
                         : psa_aead_verify(&op, out + done, len - done, &last, tag, 16);
    psa_aead_abort(&op);
    psa_destroy_key(id);
    return status == PSA_SUCCESS && done + last == len && (!encrypt || tag_len == 16) ? 0 : -1;
}

int lan_crypto_random(uint8_t *out, size_t len)
{
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    return psa_generate_random(out, len) == PSA_SUCCESS ? 0 : -1;
}
