/* Mac build (finding 075): the SHA-256 part of Windows' BCrypt the port uses, over the native
   host's CommonCrypto. */
#ifndef DHD_MAC_BCRYPT_H
#define DHD_MAC_BCRYPT_H
#include <windows.h>

MAC_IMPORT(sha256_new) uint32_t mac_sha256_new(void);
MAC_IMPORT(sha256_add) void mac_sha256_add(uint32_t hash, const void *data, uint32_t size);
MAC_IMPORT(sha256_end) void mac_sha256_end(uint32_t hash, uint8_t digest[32]);

typedef void *BCRYPT_ALG_HANDLE;
typedef uintptr_t BCRYPT_HASH_HANDLE;
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
static inline LONG BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *alg, const void *id, const void *impl, ULONG flags)
{
    (void)id; (void)impl; (void)flags;
    *alg = (void *)1;
    return 0;
}
static inline LONG BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG flags) { (void)alg; (void)flags; return 0; }
static inline LONG BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE *h, void *object, ULONG size,
                                    void *secret, ULONG secret_size, ULONG flags)
{
    (void)alg; (void)object; (void)size; (void)secret; (void)secret_size; (void)flags;
    *h = mac_sha256_new();
    return 0;
}
static inline LONG BCryptHashData(BCRYPT_HASH_HANDLE h, const void *data, ULONG size, ULONG flags)
{
    (void)flags;
    mac_sha256_add((uint32_t)h, data, size);
    return 0;
}
static inline LONG BCryptFinishHash(BCRYPT_HASH_HANDLE h, uint8_t *digest, ULONG size, ULONG flags)
{
    (void)flags;
    if (size != 32) return -1;
    mac_sha256_end((uint32_t)h, digest);
    return 0;
}
static inline LONG BCryptDestroyHash(BCRYPT_HASH_HANDLE h) { (void)h; return 0; }
#endif
