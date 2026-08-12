#ifndef _NESTOR_CRYPTO_H
#define _NESTOR_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

extern const uint8_t DEFAULT_PUB_KEY[32];
extern const uint8_t DEFAULT_PRIV_KEY[32];

int32_t crypto_sign_binary(const uint8_t *private_key, const uint8_t *msg, size_t msg_len, uint8_t *sig_out);
int32_t crypto_verify_binary(const uint8_t *public_key, const uint8_t *msg, size_t msg_len, const uint8_t *sig);

#endif // _NESTOR_CRYPTO_H
