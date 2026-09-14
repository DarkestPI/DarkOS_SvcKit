#include <svc_crypto.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

extern "C" int svc_crypto_c_api_smoke(void);

namespace {

int failures = 0;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void testSha256() {
    constexpr std::array<uint8_t, SVC_CRYPTO_SHA256_SIZE> expected{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    std::array<uint8_t, SVC_CRYPTO_SHA256_SIZE> digest{};
    CHECK(svc_crypto_sha256("abc", 3, digest.data()) == SVC_CRYPTO_OK);
    CHECK(digest == expected);
}

void testRandom() {
    std::array<uint8_t, 32> first{};
    std::array<uint8_t, 32> second{};
    CHECK(svc_crypto_random(first.data(), first.size()) == SVC_CRYPTO_OK);
    CHECK(svc_crypto_random(second.data(), second.size()) == SVC_CRYPTO_OK);
    CHECK(first != second);
}

void testAesGcm() {
    std::array<uint8_t, SVC_CRYPTO_AES256_KEY_SIZE> key{};
    std::array<uint8_t, SVC_CRYPTO_GCM_NONCE_SIZE> nonce{};
    std::array<uint8_t, SVC_CRYPTO_GCM_TAG_SIZE> tag{};
    constexpr char plaintext[] = "wifi-password";
    constexpr char aad[] = "wifi.password";
    std::array<uint8_t, sizeof(plaintext) - 1> ciphertext{};
    std::array<uint8_t, sizeof(plaintext) - 1> decrypted{};

    CHECK(svc_crypto_random(key.data(), key.size()) == SVC_CRYPTO_OK);
    CHECK(svc_crypto_random(nonce.data(), nonce.size()) == SVC_CRYPTO_OK);
    CHECK(svc_crypto_aes256_gcm_encrypt(key.data(), nonce.data(), plaintext, sizeof(plaintext) - 1, aad,
                                        sizeof(aad) - 1, ciphertext.data(), tag.data()) == SVC_CRYPTO_OK);
    CHECK(svc_crypto_aes256_gcm_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(), aad,
                                        sizeof(aad) - 1, tag.data(), decrypted.data()) == SVC_CRYPTO_OK);
    CHECK(std::memcmp(decrypted.data(), plaintext, decrypted.size()) == 0);

    tag[0] ^= 1U;
    CHECK(svc_crypto_aes256_gcm_decrypt(key.data(), nonce.data(), ciphertext.data(), ciphertext.size(), aad,
                                        sizeof(aad) - 1, tag.data(), decrypted.data()) == SVC_CRYPTO_AUTH_FAILED);
    CHECK(std::all_of(decrypted.begin(), decrypted.end(), [](uint8_t value) { return value == 0; }));

    svc_crypto_zeroize(key.data(), key.size());
}

} // namespace

int main() {
    CHECK(svc_crypto_c_api_smoke() == SVC_CRYPTO_OK);
    testSha256();
    testRandom();
    testAesGcm();

    if (failures != 0) {
        std::fprintf(stderr, "%d svc_crypto host test(s) failed\n", failures);
        return 1;
    }
    std::puts("svc_crypto host tests passed");
    return 0;
}
