#include "crypto.h"
#include "error_codes.h"
#include <openssl/evp.h>
#include <string.h>

const uint8_t DEFAULT_PUB_KEY[32] = {
  0x01, 0x46, 0x1D, 0x96, 0x63, 0x74, 0xAA, 0x56, 0x82, 0xDF, 0x58, 0xEC, 0xE3, 0x72, 0x76, 0xB5, 0x9F, 0x76, 0x4C, 0x5E, 0x94, 0xE9, 0xB6, 0x65, 0xBE, 0x9F, 0xD4, 0xBE, 0xE4, 0xAF, 0x1A, 0x01
};

const uint8_t DEFAULT_PRIV_KEY[32] = {
  0x48, 0x50, 0xED, 0xF7, 0xEA, 0x4D, 0x8E, 0x36, 0x2E, 0x24, 0x48, 0xDF, 0x96, 0xCE, 0xF8, 0x5C, 0xBF, 0x78, 0x29, 0xCD, 0xB4, 0xEF, 0xF8, 0xC2, 0x78, 0x01, 0x57, 0xA2, 0x99, 0x31, 0xA8, 0x1E
};

int32_t crypto_sign_binary(const uint8_t *private_key, const uint8_t *msg, size_t msg_len, uint8_t *sig_out) {
  /*#region*/
  // We use EVP_PKEY_new_raw_private_key to wrap the raw 32-byte Ed25519 private key
  // into an EVP_PKEY structure without performing disk I/O.
  EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key, 32);
  if (!pkey) {
    return -10; // ERR_VM_ILLEGAL_INSTRUCTION or equivalent crypto failure
  }

  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
  if (!md_ctx) {
    EVP_PKEY_free(pkey);
    return -10;
  }

  // Ed25519 does not use a digest algorithm, so md parameter is NULL.
  if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return -10;
  }

  size_t sig_len = 64;
  if (EVP_DigestSign(md_ctx, sig_out, &sig_len, msg, msg_len) <= 0 || sig_len != 64) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return -10;
  }

  EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(pkey);
  return 0; // ERR_SUCCESS
  /*#endregion*/
}

int32_t crypto_verify_binary(const uint8_t *public_key, const uint8_t *msg, size_t msg_len, const uint8_t *sig) {
  /*#region*/
  // Wraps the raw 32-byte Ed25519 public key.
  EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, 32);
  if (!pkey) {
    return -10; // Failure
  }

  EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
  if (!md_ctx) {
    EVP_PKEY_free(pkey);
    return -10;
  }

  if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) <= 0) {
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return -10;
  }

  // EVP_DigestVerify returns 1 on successful verification, 0 on mismatch, or <0 on error.
  int verify_res = EVP_DigestVerify(md_ctx, sig, 64, msg, msg_len);
  
  EVP_MD_CTX_free(md_ctx);
  EVP_PKEY_free(pkey);

  if (verify_res == 1) {
    return 0; // ERR_SUCCESS
  } else {
    return -10; // ERR_VM_ILLEGAL_INSTRUCTION
  }
  /*#endregion*/
}
