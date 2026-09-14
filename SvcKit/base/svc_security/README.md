# svc_security

`svc_security` 是 SvcKit 的安全基础组件，由密码运算和 Secret 管理两个独立模块
组成：

```text
svc_security/
├── svc_crypto/      # 摘要、随机数和认证加密
└── svc_keystore/    # Wi-Fi 密码、Token 等 Secret 的加密存储
```

公开接口兼容 C17 和 C++17。密码算法由 OpenSSL `libcrypto` 提供，组件自身不
实现密码算法。

## 启用组件

安全组件默认关闭，避免没有 OpenSSL 的交叉编译环境受到影响：

```bash
cmake -S applications/rv1126b_ipc \
    -B build/security \
    -DDARKOS_BUILD_SECURITY_COMPONENTS=ON
cmake --build build/security -j
```

应用链接 `DarkOS::SvcKit` 或 `DarkOS::Base` 后可以使用全部安全组件。也可以只
链接需要的目标：

```cmake
target_link_libraries(my_app PRIVATE DarkOS::Crypto)
target_link_libraries(my_app PRIVATE DarkOS::KeyStore)
# 或一次链接两者
target_link_libraries(my_app PRIVATE DarkOS::Security)
```

## svc_crypto

`svc_crypto` 当前提供：

- 密码学安全随机数；
- SHA-256 摘要；
- AES-256-GCM 认证加密和解密；
- 敏感内存清理。

AES-GCM 同时保护数据的机密性和完整性。Nonce 在同一密钥下不得重复；通常应为
每次加密生成新的随机 Nonce，并与密文一起保存。

```c
#include <svc_crypto.h>

uint8_t key[SVC_CRYPTO_AES256_KEY_SIZE];
uint8_t nonce[SVC_CRYPTO_GCM_NONCE_SIZE];
uint8_t tag[SVC_CRYPTO_GCM_TAG_SIZE];
uint8_t ciphertext[128];

svc_crypto_random(key, sizeof(key));
svc_crypto_random(nonce, sizeof(nonce));

svc_crypto_aes256_gcm_encrypt(
    key,
    nonce,
    plaintext,
    plaintext_size,
    "config.name",
    sizeof("config.name") - 1,
    ciphertext,
    tag);

svc_crypto_zeroize(key, sizeof(key));
```

业务层保存加密数据时必须同时保存 Nonce、认证标签和密文。认证标签校验失败时，
解密返回 `SVC_CRYPTO_AUTH_FAILED`，并清理已经写入的明文缓冲区。

## svc_keystore

`svc_keystore` 当前提供文件型 Secret 仓库，适合保存：

- Wi-Fi 密码；
- 云平台 Token；
- MQTT 密码；
- API Secret；
- 其他必须恢复为明文使用的小型机密数据。

打开仓库：

```c
#include <stdlib.h>
#include <svc_keystore.h>

svc_keystore_t *keystore = NULL;
svc_keystore_result_t result =
    svc_keystore_open("/var/lib/darkos/keystore", &keystore);
if (result != SVC_KEYSTORE_OK) {
    /* 处理错误 */
}
```

保存 Secret：

```c
const char password[] = "my-wifi-password";

result = svc_keystore_store_secret(
    keystore,
    "wifi.password",
    password,
    sizeof(password) - 1);
```

读取 Secret 需要先查询长度，再由调用方提供缓冲区：

```c
size_t size = 0;
result = svc_keystore_load_secret(
    keystore, "wifi.password", NULL, &size);

if (result == SVC_KEYSTORE_BUFFER_TOO_SMALL) {
    uint8_t *password = malloc(size + 1);
    if (password != NULL) {
        size_t capacity = size;
        result = svc_keystore_load_secret(
            keystore, "wifi.password", password, &capacity);

        if (result == SVC_KEYSTORE_OK) {
            password[capacity] = '\0';
            /* 使用 password，禁止输出到日志 */
        }

        svc_crypto_zeroize(password, size + 1);
        free(password);
    }
}
```

查询和删除：

```c
int exists = svc_keystore_contains(keystore, "wifi.password");
svc_keystore_remove(keystore, "wifi.password");
svc_keystore_close(keystore);
```

条目名只允许英文字母、数字、点、下划线和连字符，最大长度为 128 字节，避免
目录穿越。

## 文件布局与权限

```text
/var/lib/darkos/keystore/
├── master.key                  # 32 字节主密钥，权限 0600
└── entries/                    # 权限 0700
    ├── wifi.password.secret
    └── cloud.token.secret
```

每个 Secret 使用独立随机 Nonce，通过 AES-256-GCM 加密。条目名作为附加认证
数据参与校验，因此密文不能被直接改名复用。同名条目更新时使用临时文件和
`rename()` 原子替换。

### 文件后端的安全边界

当前第一阶段实现把 `master.key` 和密文存放在同一设备上，主要防止配置文件被
单独复制、误传或直接查看，不抵御已经获得 root 权限或能够读取整个仓库目录的
攻击者。

生产设备应进一步把主密钥迁移到平台安全后端，例如：

- TPM；
- TEE 或受保护存储；
- 板载安全芯片；
- Linux Kernel Keyring；
- 芯片厂商提供的不可导出密钥接口。

`svc_keystore` 的业务接口可以保持不变，只替换底层主密钥提供方式。

## 使用注意事项

- 不要把密钥、密码或 Token 写入日志；
- Secret 使用完后调用 `svc_crypto_zeroize()` 清理内存；
- 仓库目录不要放在 Web 静态目录、普通配置目录或可移动介质；
- 不要把 `master.key` 打包进固件或提交到 Git；
- AES-GCM 的 Nonce 不得在同一密钥下重复；
- 定期轮换云平台 Token，并为 Token 设置最小权限；
- 当前单个 Secret 最大为 16 MiB，不适合加密视频或大型文件。

## 宿主机测试

```bash
cmake -S applications/rv1126b_ipc \
    -B build/security-test \
    -DDARKOS_BUILD_SECURITY_COMPONENTS=ON \
    -DDARKOS_BUILD_TESTS=ON \
    -DDARKOS_ENABLE_OUTPUT_LAYOUT=OFF

cmake --build build/security-test \
    --target svc_crypto_host_test svc_keystore_host_test -j

ctest --test-dir build/security-test \
    -R 'svc_(crypto|keystore)\.host' \
    --output-on-failure
```

测试覆盖 SHA-256、随机数、AES-GCM 往返与篡改检测、C17 接口编译、Secret
持久化、文件权限、非法名称、删除和密文篡改检测。
