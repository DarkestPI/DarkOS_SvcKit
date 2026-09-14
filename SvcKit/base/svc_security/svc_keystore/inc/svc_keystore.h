/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SVC_KEYSTORE_H
#define SVC_KEYSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct svc_keystore svc_keystore_t;

typedef enum {
    SVC_KEYSTORE_OK = 0,                /**< 操作成功。 */
    SVC_KEYSTORE_INVALID_ARGUMENT = -1, /**< 参数非法，或条目名称不符合命名规则。 */
    SVC_KEYSTORE_NOT_FOUND = -2,        /**< 指定的 Secret 条目不存在。 */
    SVC_KEYSTORE_BUFFER_TOO_SMALL = -3, /**< 输出缓冲区容量不足。 */
    SVC_KEYSTORE_AUTH_FAILED = -4,      /**< Secret 认证解密失败。 */
    SVC_KEYSTORE_IO_ERROR = -5,         /**< 文件系统操作失败。 */
    SVC_KEYSTORE_CRYPTO_ERROR = -6,     /**< 随机数生成或加解密失败。 */
    SVC_KEYSTORE_OUT_OF_MEMORY = -7,    /**< 分配仓库对象或临时缓冲区失败。 */
} svc_keystore_result_t;

/**
 * 打开文件型 Secret 仓库。目录不存在时以 0700 权限创建，并在首次打开时生成
 * 0600 权限的主密钥文件。
 */
svc_keystore_result_t svc_keystore_open(const char *directory, svc_keystore_t **keystore);

/** 关闭仓库并清理内存中的主密钥。 */
void svc_keystore_close(svc_keystore_t *keystore);

/** 加密保存 Secret；同名条目存在时执行原子替换。 */
svc_keystore_result_t svc_keystore_store_secret(svc_keystore_t *keystore, const char *name, const void *secret,
                                                size_t secret_size);

/**
 * 读取并解密 Secret。首次可传入 output=NULL 查询长度；空间不足时通过
 * output_size 返回所需长度。
 */
svc_keystore_result_t svc_keystore_load_secret(svc_keystore_t *keystore, const char *name, void *output,
                                               size_t *output_size);

/** 判断条目是否存在：存在返回 1，不存在返回 0，参数非法返回负数。 */
int svc_keystore_contains(svc_keystore_t *keystore, const char *name);

/** 删除指定条目。 */
svc_keystore_result_t svc_keystore_remove(svc_keystore_t *keystore, const char *name);

/** 返回错误码对应的静态字符串。 */
const char *svc_keystore_result_string(svc_keystore_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* SVC_KEYSTORE_H */
