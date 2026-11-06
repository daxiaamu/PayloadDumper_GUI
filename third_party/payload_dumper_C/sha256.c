#include "sha256.h"
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(x) ((x) >= 0)
#endif

int sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status;
    int ret = -1;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) return -1;

    status = BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return -1;
    }

    size_t offset = 0;
    while (offset < len) {
        ULONG chunk = (ULONG)((len - offset) > 0x40000000ULL ?
                              0x40000000ULL : (len - offset));
        status = BCryptHashData(hHash, (PUCHAR)(data + offset), chunk, 0);
        if (!NT_SUCCESS(status)) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return -1;
        }
        offset += chunk;
    }

    status = BCryptFinishHash(hHash, out, 32, 0);
    if (!NT_SUCCESS(status)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return -1;
    }

    ret = 0;
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ret;
}
