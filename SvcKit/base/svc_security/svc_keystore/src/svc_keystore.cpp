/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <svc_crypto.h>
#include <svc_keystore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::array<uint8_t, 8> kMagic{'S', 'V', 'C', 'K', 'S', '0', '1', 0};
constexpr size_t kLengthSize = 4;
constexpr size_t kHeaderSize = kMagic.size() + kLengthSize + SVC_CRYPTO_GCM_NONCE_SIZE + SVC_CRYPTO_GCM_TAG_SIZE;
constexpr size_t kMaximumSecretSize = 16U * 1024U * 1024U;

#ifdef O_NOFOLLOW
constexpr int kNoFollow = O_NOFOLLOW;
#else
constexpr int kNoFollow = 0;
#endif

bool validName(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    size_t length = 0;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(name); *cursor != '\0'; ++cursor) {
        if (++length > 128 || !(std::isalnum(*cursor) || *cursor == '_' || *cursor == '-' || *cursor == '.')) {
            return false;
        }
    }
    return std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0;
}

bool writeAll(int file, const uint8_t *data, size_t size) {
    while (size != 0) {
        const ssize_t written = ::write(file, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        data += static_cast<size_t>(written);
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool readAll(int file, uint8_t *data, size_t size) {
    while (size != 0) {
        const ssize_t count = ::read(file, data, size);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        data += static_cast<size_t>(count);
        size -= static_cast<size_t>(count);
    }
    return true;
}

bool readFile(const std::string &path, std::vector<uint8_t> &content, int &error) {
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | kNoFollow);
    if (file < 0) {
        error = errno;
        return false;
    }

    struct stat status {};
    bool success = ::fstat(file, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0 &&
                   static_cast<uint64_t>(status.st_size) <= kHeaderSize + kMaximumSecretSize;
    if (success) {
        content.resize(static_cast<size_t>(status.st_size));
        success = readAll(file, content.data(), content.size());
    }
    error = success ? 0 : EIO;
    ::close(file);
    return success;
}

bool writeFileAtomically(const std::string &path, const uint8_t *data, size_t size) {
    const std::string temporary = path + ".tmp." + std::to_string(::getpid());
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | kNoFollow, S_IRUSR | S_IWUSR);
    if (file < 0) {
        return false;
    }

    bool success = writeAll(file, data, size) && ::fsync(file) == 0;
    if (::close(file) != 0) {
        success = false;
    }
    if (success && ::rename(temporary.c_str(), path.c_str()) != 0) {
        success = false;
    }
    if (!success) {
        ::unlink(temporary.c_str());
    }
    return success;
}

void encodeLength(uint32_t value, uint8_t *output) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
    output[2] = static_cast<uint8_t>(value >> 16U);
    output[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t decodeLength(const uint8_t *input) {
    return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8U) |
           (static_cast<uint32_t>(input[2]) << 16U) | (static_cast<uint32_t>(input[3]) << 24U);
}

svc_keystore_result_t cryptoResult(svc_crypto_result_t result) {
    if (result == SVC_CRYPTO_AUTH_FAILED) {
        return SVC_KEYSTORE_AUTH_FAILED;
    }
    if (result == SVC_CRYPTO_OUT_OF_MEMORY) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    }
    return result == SVC_CRYPTO_OK ? SVC_KEYSTORE_OK : SVC_KEYSTORE_CRYPTO_ERROR;
}

} // namespace

struct svc_keystore {
    std::string root;
    std::string entries;
    std::array<uint8_t, SVC_CRYPTO_AES256_KEY_SIZE> masterKey{};
    std::mutex mutex;
};

namespace {

svc_keystore_result_t loadOrCreateMasterKey(svc_keystore &keystore) {
    const std::string path = keystore.root + "/master.key";
    int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | kNoFollow);
    if (file >= 0) {
        struct stat status {};
        const bool valid = ::fstat(file, &status) == 0 && S_ISREG(status.st_mode) &&
                           status.st_size == SVC_CRYPTO_AES256_KEY_SIZE && ::fchmod(file, S_IRUSR | S_IWUSR) == 0 &&
                           readAll(file, keystore.masterKey.data(), keystore.masterKey.size());
        ::close(file);
        return valid ? SVC_KEYSTORE_OK : SVC_KEYSTORE_IO_ERROR;
    }
    if (errno != ENOENT) {
        return SVC_KEYSTORE_IO_ERROR;
    }

    const auto randomResult = svc_crypto_random(keystore.masterKey.data(), keystore.masterKey.size());
    if (randomResult != SVC_CRYPTO_OK) {
        return cryptoResult(randomResult);
    }

    file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | kNoFollow, S_IRUSR | S_IWUSR);
    if (file < 0) {
        if (errno == EEXIST) {
            svc_crypto_zeroize(keystore.masterKey.data(), keystore.masterKey.size());
            return loadOrCreateMasterKey(keystore);
        }
        return SVC_KEYSTORE_IO_ERROR;
    }

    bool success = writeAll(file, keystore.masterKey.data(), keystore.masterKey.size()) && ::fsync(file) == 0;
    if (::close(file) != 0) {
        success = false;
    }
    if (!success) {
        ::unlink(path.c_str());
        svc_crypto_zeroize(keystore.masterKey.data(), keystore.masterKey.size());
        return SVC_KEYSTORE_IO_ERROR;
    }
    return SVC_KEYSTORE_OK;
}

std::string entryPath(const svc_keystore &keystore, const char *name) {
    return keystore.entries + "/" + name + ".secret";
}

} // namespace

extern "C" {

svc_keystore_result_t svc_keystore_open(const char *directory, svc_keystore_t **keystore) {
    if (directory == nullptr || directory[0] == '\0' || keystore == nullptr) {
        return SVC_KEYSTORE_INVALID_ARGUMENT;
    }
    *keystore = nullptr;

    try {
        std::error_code error;
        const std::filesystem::path root(directory);
        if (std::filesystem::exists(root, error)) {
            if (error || std::filesystem::is_symlink(std::filesystem::symlink_status(root, error)) || error ||
                !std::filesystem::is_directory(root, error)) {
                return SVC_KEYSTORE_IO_ERROR;
            }
        } else if (!std::filesystem::create_directories(root, error) || error) {
            return SVC_KEYSTORE_IO_ERROR;
        }
        if (::chmod(root.c_str(), S_IRWXU) != 0) {
            return SVC_KEYSTORE_IO_ERROR;
        }

        const std::filesystem::path entries = root / "entries";
        if (std::filesystem::exists(entries, error)) {
            if (error || std::filesystem::is_symlink(std::filesystem::symlink_status(entries, error)) || error ||
                !std::filesystem::is_directory(entries, error)) {
                return SVC_KEYSTORE_IO_ERROR;
            }
        } else if (!std::filesystem::create_directory(entries, error) || error) {
            return SVC_KEYSTORE_IO_ERROR;
        }
        if (::chmod(entries.c_str(), S_IRWXU) != 0) {
            return SVC_KEYSTORE_IO_ERROR;
        }

        auto instance = std::unique_ptr<svc_keystore>(new (std::nothrow) svc_keystore);
        if (!instance) {
            return SVC_KEYSTORE_OUT_OF_MEMORY;
        }
        instance->root = root.string();
        instance->entries = entries.string();

        const auto result = loadOrCreateMasterKey(*instance);
        if (result != SVC_KEYSTORE_OK) {
            return result;
        }
        *keystore = instance.release();
        return SVC_KEYSTORE_OK;
    } catch (const std::bad_alloc &) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    } catch (...) {
        return SVC_KEYSTORE_IO_ERROR;
    }
}

void svc_keystore_close(svc_keystore_t *keystore) {
    if (keystore != nullptr) {
        svc_crypto_zeroize(keystore->masterKey.data(), keystore->masterKey.size());
        delete keystore;
    }
}

svc_keystore_result_t svc_keystore_store_secret(svc_keystore_t *keystore, const char *name, const void *secret,
                                                size_t secret_size) {
    if (keystore == nullptr || !validName(name) || (secret_size != 0 && secret == nullptr) ||
        secret_size > kMaximumSecretSize || secret_size > std::numeric_limits<uint32_t>::max()) {
        return SVC_KEYSTORE_INVALID_ARGUMENT;
    }

    try {
        std::lock_guard<std::mutex> lock(keystore->mutex);
        std::array<uint8_t, SVC_CRYPTO_GCM_NONCE_SIZE> nonce{};
        std::array<uint8_t, SVC_CRYPTO_GCM_TAG_SIZE> tag{};
        std::vector<uint8_t> ciphertext(secret_size);

        auto result = svc_crypto_random(nonce.data(), nonce.size());
        if (result != SVC_CRYPTO_OK) {
            return cryptoResult(result);
        }
        result = svc_crypto_aes256_gcm_encrypt(keystore->masterKey.data(), nonce.data(), secret, secret_size, name,
                                               std::strlen(name), ciphertext.data(), tag.data());
        if (result != SVC_CRYPTO_OK) {
            return cryptoResult(result);
        }

        std::vector<uint8_t> file(kHeaderSize + secret_size);
        size_t offset = 0;
        std::copy(kMagic.begin(), kMagic.end(), file.begin());
        offset += kMagic.size();
        encodeLength(static_cast<uint32_t>(secret_size), file.data() + offset);
        offset += kLengthSize;
        std::copy(nonce.begin(), nonce.end(), file.begin() + offset);
        offset += nonce.size();
        std::copy(tag.begin(), tag.end(), file.begin() + offset);
        offset += tag.size();
        std::copy(ciphertext.begin(), ciphertext.end(), file.begin() + offset);

        const bool written = writeFileAtomically(entryPath(*keystore, name), file.data(), file.size());
        svc_crypto_zeroize(ciphertext.data(), ciphertext.size());
        svc_crypto_zeroize(file.data(), file.size());
        return written ? SVC_KEYSTORE_OK : SVC_KEYSTORE_IO_ERROR;
    } catch (const std::bad_alloc &) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    } catch (...) {
        return SVC_KEYSTORE_IO_ERROR;
    }
}

svc_keystore_result_t svc_keystore_load_secret(svc_keystore_t *keystore, const char *name, void *output,
                                               size_t *output_size) {
    if (keystore == nullptr || !validName(name) || output_size == nullptr) {
        return SVC_KEYSTORE_INVALID_ARGUMENT;
    }

    try {
        std::lock_guard<std::mutex> lock(keystore->mutex);
        std::vector<uint8_t> file;
        int error = 0;
        if (!readFile(entryPath(*keystore, name), file, error)) {
            return error == ENOENT ? SVC_KEYSTORE_NOT_FOUND : SVC_KEYSTORE_IO_ERROR;
        }
        if (file.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), file.begin())) {
            return SVC_KEYSTORE_IO_ERROR;
        }

        size_t offset = kMagic.size();
        const size_t secretSize = decodeLength(file.data() + offset);
        offset += kLengthSize;
        if (secretSize > kMaximumSecretSize || file.size() != kHeaderSize + secretSize) {
            return SVC_KEYSTORE_IO_ERROR;
        }

        if (output == nullptr || *output_size < secretSize) {
            *output_size = secretSize;
            return SVC_KEYSTORE_BUFFER_TOO_SMALL;
        }

        const uint8_t *nonce = file.data() + offset;
        offset += SVC_CRYPTO_GCM_NONCE_SIZE;
        const uint8_t *tag = file.data() + offset;
        offset += SVC_CRYPTO_GCM_TAG_SIZE;
        const uint8_t *ciphertext = file.data() + offset;

        const auto result = svc_crypto_aes256_gcm_decrypt(keystore->masterKey.data(), nonce, ciphertext, secretSize,
                                                          name, std::strlen(name), tag, static_cast<uint8_t *>(output));
        svc_crypto_zeroize(file.data(), file.size());
        if (result != SVC_CRYPTO_OK) {
            *output_size = 0;
            return cryptoResult(result);
        }
        *output_size = secretSize;
        return SVC_KEYSTORE_OK;
    } catch (const std::bad_alloc &) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    } catch (...) {
        return SVC_KEYSTORE_IO_ERROR;
    }
}

int svc_keystore_contains(svc_keystore_t *keystore, const char *name) {
    if (keystore == nullptr || !validName(name)) {
        return SVC_KEYSTORE_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(keystore->mutex);
        struct stat status {};
        if (::lstat(entryPath(*keystore, name).c_str(), &status) == 0) {
            return S_ISREG(status.st_mode) ? 1 : SVC_KEYSTORE_IO_ERROR;
        }
        return errno == ENOENT ? 0 : SVC_KEYSTORE_IO_ERROR;
    } catch (...) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    }
}

svc_keystore_result_t svc_keystore_remove(svc_keystore_t *keystore, const char *name) {
    if (keystore == nullptr || !validName(name)) {
        return SVC_KEYSTORE_INVALID_ARGUMENT;
    }
    try {
        std::lock_guard<std::mutex> lock(keystore->mutex);
        if (::unlink(entryPath(*keystore, name).c_str()) == 0) {
            return SVC_KEYSTORE_OK;
        }
        return errno == ENOENT ? SVC_KEYSTORE_NOT_FOUND : SVC_KEYSTORE_IO_ERROR;
    } catch (const std::bad_alloc &) {
        return SVC_KEYSTORE_OUT_OF_MEMORY;
    } catch (...) {
        return SVC_KEYSTORE_IO_ERROR;
    }
}

const char *svc_keystore_result_string(svc_keystore_result_t result) {
    switch (result) {
        case SVC_KEYSTORE_OK:
            return "success";
        case SVC_KEYSTORE_INVALID_ARGUMENT:
            return "invalid argument";
        case SVC_KEYSTORE_NOT_FOUND:
            return "not found";
        case SVC_KEYSTORE_BUFFER_TOO_SMALL:
            return "buffer too small";
        case SVC_KEYSTORE_AUTH_FAILED:
            return "authentication failed";
        case SVC_KEYSTORE_IO_ERROR:
            return "I/O error";
        case SVC_KEYSTORE_CRYPTO_ERROR:
            return "cryptography error";
        case SVC_KEYSTORE_OUT_OF_MEMORY:
            return "out of memory";
    }
    return "unknown error";
}

} // extern "C"
