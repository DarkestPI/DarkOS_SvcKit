/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SVC_CRYPTO_H
#define SVC_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVC_CRYPTO_SHA256_SIZE 32U
#define SVC_CRYPTO_AES256_KEY_SIZE 32U
#define SVC_CRYPTO_GCM_NONCE_SIZE 12U
#define SVC_CRYPTO_GCM_TAG_SIZE 16U

typedef enum {
    SVC_CRYPTO_OK = 0,                /**< 操作成功。 */
    SVC_CRYPTO_INVALID_ARGUMENT = -1, /**< 参数为空、长度非法或缓冲区不符合要求。 */
    SVC_CRYPTO_OUT_OF_MEMORY = -2,    /**< 创建密码运算上下文时内存不足。 */
    SVC_CRYPTO_AUTH_FAILED = -3,      /**< AES-GCM 认证标签校验失败。 */
    SVC_CRYPTO_BACKEND_ERROR = -4,    /**< 底层密码库或系统随机源执行失败。 */
} svc_crypto_result_t;

/** 使用系统密码学随机源生成随机数据。 */
svc_crypto_result_t svc_crypto_random(uint8_t *output, size_t output_size);

/** 计算 SHA-256 摘要，digest 必须至少有 32 字节。 */
svc_crypto_result_t svc_crypto_sha256(const void *data, size_t data_size, uint8_t digest[SVC_CRYPTO_SHA256_SIZE]);

/** 使用 AES-256-GCM 加密，密文长度与明文长度相同。 */
svc_crypto_result_t svc_crypto_aes256_gcm_encrypt(const uint8_t key[SVC_CRYPTO_AES256_KEY_SIZE],
                                                  const uint8_t nonce[SVC_CRYPTO_GCM_NONCE_SIZE], const void *plaintext,
                                                  size_t plaintext_size, const void *aad, size_t aad_size,
                                                  uint8_t *ciphertext, uint8_t tag[SVC_CRYPTO_GCM_TAG_SIZE]);

/** 使用 AES-256-GCM 解密并校验认证标签。 */
svc_crypto_result_t svc_crypto_aes256_gcm_decrypt(const uint8_t key[SVC_CRYPTO_AES256_KEY_SIZE],
                                                  const uint8_t nonce[SVC_CRYPTO_GCM_NONCE_SIZE],
                                                  const void *ciphertext, size_t ciphertext_size, const void *aad,
                                                  size_t aad_size, const uint8_t tag[SVC_CRYPTO_GCM_TAG_SIZE],
                                                  uint8_t *plaintext);

/** 以不易被编译器优化掉的方式清理敏感内存。 */
void svc_crypto_zeroize(void *data, size_t data_size);

#ifdef __cplusplus
}
#endif

#endif /* SVC_CRYPTO_H */
