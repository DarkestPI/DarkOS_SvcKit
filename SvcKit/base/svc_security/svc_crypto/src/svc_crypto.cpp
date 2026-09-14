/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <svc_crypto.h>

#include <climits>
#include <memory>

namespace {

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

bool validBuffer(const void *buffer, size_t size) {
    return size == 0 || buffer != nullptr;
}

bool validOperationSize(size_t size) {
    return size <= static_cast<size_t>(INT_MAX);
}

} // namespace

extern "C" {

svc_crypto_result_t svc_crypto_random(uint8_t *output, size_t output_size) {
    if (!validBuffer(output, output_size) || !validOperationSize(output_size)) {
        return SVC_CRYPTO_INVALID_ARGUMENT;
    }
    if (output_size == 0) {
        return SVC_CRYPTO_OK;
    }
    return RAND_bytes(output, static_cast<int>(output_size)) == 1 ? SVC_CRYPTO_OK : SVC_CRYPTO_BACKEND_ERROR;
}

svc_crypto_result_t svc_crypto_sha256(const void *data, size_t data_size, uint8_t digest[SVC_CRYPTO_SHA256_SIZE]) {
    if (!validBuffer(data, data_size) || digest == nullptr || !validOperationSize(data_size)) {
        return SVC_CRYPTO_INVALID_ARGUMENT;
    }

    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) {
        return SVC_CRYPTO_OUT_OF_MEMORY;
    }

    unsigned int digestSize = 0;
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        (data_size != 0 && EVP_DigestUpdate(context.get(), data, data_size) != 1) ||
        EVP_DigestFinal_ex(context.get(), digest, &digestSize) != 1 || digestSize != SVC_CRYPTO_SHA256_SIZE) {
        return SVC_CRYPTO_BACKEND_ERROR;
    }
    return SVC_CRYPTO_OK;
}

svc_crypto_result_t svc_crypto_aes256_gcm_encrypt(const uint8_t key[SVC_CRYPTO_AES256_KEY_SIZE],
                                                  const uint8_t nonce[SVC_CRYPTO_GCM_NONCE_SIZE], const void *plaintext,
                                                  size_t plaintext_size, const void *aad, size_t aad_size,
                                                  uint8_t *ciphertext, uint8_t tag[SVC_CRYPTO_GCM_TAG_SIZE]) {
    if (key == nullptr || nonce == nullptr || tag == nullptr || !validBuffer(plaintext, plaintext_size) ||
        !validBuffer(ciphertext, plaintext_size) || !validBuffer(aad, aad_size) ||
        !validOperationSize(plaintext_size) || !validOperationSize(aad_size)) {
        return SVC_CRYPTO_INVALID_ARGUMENT;
    }

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context) {
        return SVC_CRYPTO_OUT_OF_MEMORY;
    }

    int length = 0;
    int finalLength = 0;
    uint8_t finalByte = 0;
    if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, SVC_CRYPTO_GCM_NONCE_SIZE, nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key, nonce) != 1 ||
        (aad_size != 0 && EVP_EncryptUpdate(context.get(), nullptr, &length, static_cast<const uint8_t *>(aad),
                                            static_cast<int>(aad_size)) != 1) ||
        (plaintext_size != 0 &&
         EVP_EncryptUpdate(context.get(), ciphertext, &length, static_cast<const uint8_t *>(plaintext),
                           static_cast<int>(plaintext_size)) != 1) ||
        EVP_EncryptFinal_ex(context.get(), plaintext_size != 0 ? ciphertext + length : &finalByte, &finalLength) != 1 ||
        static_cast<size_t>(length + finalLength) != plaintext_size ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, SVC_CRYPTO_GCM_TAG_SIZE, tag) != 1) {
        if (ciphertext != nullptr) {
            svc_crypto_zeroize(ciphertext, plaintext_size);
        }
        svc_crypto_zeroize(tag, SVC_CRYPTO_GCM_TAG_SIZE);
        return SVC_CRYPTO_BACKEND_ERROR;
    }
    return SVC_CRYPTO_OK;
}

svc_crypto_result_t svc_crypto_aes256_gcm_decrypt(const uint8_t key[SVC_CRYPTO_AES256_KEY_SIZE],
                                                  const uint8_t nonce[SVC_CRYPTO_GCM_NONCE_SIZE],
                                                  const void *ciphertext, size_t ciphertext_size, const void *aad,
                                                  size_t aad_size, const uint8_t tag[SVC_CRYPTO_GCM_TAG_SIZE],
                                                  uint8_t *plaintext) {
    if (key == nullptr || nonce == nullptr || tag == nullptr || !validBuffer(ciphertext, ciphertext_size) ||
        !validBuffer(plaintext, ciphertext_size) || !validBuffer(aad, aad_size) ||
        !validOperationSize(ciphertext_size) || !validOperationSize(aad_size)) {
        return SVC_CRYPTO_INVALID_ARGUMENT;
    }

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context) {
        return SVC_CRYPTO_OUT_OF_MEMORY;
    }

    int length = 0;
    int finalLength = 0;
    uint8_t finalByte = 0;
    if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, SVC_CRYPTO_GCM_NONCE_SIZE, nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key, nonce) != 1 ||
        (aad_size != 0 && EVP_DecryptUpdate(context.get(), nullptr, &length, static_cast<const uint8_t *>(aad),
                                            static_cast<int>(aad_size)) != 1) ||
        (ciphertext_size != 0 &&
         EVP_DecryptUpdate(context.get(), plaintext, &length, static_cast<const uint8_t *>(ciphertext),
                           static_cast<int>(ciphertext_size)) != 1) ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, SVC_CRYPTO_GCM_TAG_SIZE, const_cast<uint8_t *>(tag)) !=
            1) {
        if (plaintext != nullptr) {
            svc_crypto_zeroize(plaintext, ciphertext_size);
        }
        return SVC_CRYPTO_BACKEND_ERROR;
    }

    if (EVP_DecryptFinal_ex(context.get(), ciphertext_size != 0 ? plaintext + length : &finalByte, &finalLength) != 1) {
        if (plaintext != nullptr) {
            svc_crypto_zeroize(plaintext, ciphertext_size);
        }
        return SVC_CRYPTO_AUTH_FAILED;
    }

    if (static_cast<size_t>(length + finalLength) != ciphertext_size) {
        if (plaintext != nullptr) {
            svc_crypto_zeroize(plaintext, ciphertext_size);
        }
        return SVC_CRYPTO_BACKEND_ERROR;
    }
    return SVC_CRYPTO_OK;
}

void svc_crypto_zeroize(void *data, size_t data_size) {
    if (data != nullptr && data_size != 0) {
        OPENSSL_cleanse(data, data_size);
    }
}

} // extern "C"
