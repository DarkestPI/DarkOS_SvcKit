#include <svc_crypto.h>

int svc_crypto_c_api_smoke(void) {
    static const char message[] = "c-api";
    uint8_t digest[SVC_CRYPTO_SHA256_SIZE];
    return svc_crypto_sha256(message, sizeof(message) - 1, digest);
}
