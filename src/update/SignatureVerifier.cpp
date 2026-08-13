#include "SignatureVerifier.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <array>
#include <cstring>
#include <vector>

#include "GeneratedUpdateKeys.h"

namespace cheburnet::update {
namespace {

constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0x00000000L);
constexpr NTSTATUS kStatusInvalidSignature = static_cast<NTSTATUS>(0xC000A000L);

bool DecodeBase64(std::string_view encoded, std::vector<unsigned char>& output) {
    while (!encoded.empty() && (encoded.back() == '\r' || encoded.back() == '\n' ||
                                encoded.back() == ' ' || encoded.back() == '\t'))
        encoded.remove_suffix(1);
    if (encoded.empty() || encoded.size() > 256) return false;
    DWORD size = 0;
    if (!::CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size,
                                nullptr, nullptr)) return false;
    output.resize(size);
    return ::CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, output.data(), &size,
                                  nullptr, nullptr) != FALSE && (output.resize(size), true);
}

bool Sha256(std::string_view bytes, std::array<unsigned char, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, resultLength = 0;
    std::vector<unsigned char> object;
    bool ok = BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                           nullptr, 0));
    if (ok) ok = BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                                     reinterpret_cast<PUCHAR>(&objectLength),
                                                     sizeof(objectLength), &resultLength, 0));
    if (ok) {
        object.resize(objectLength);
        ok = BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                                               nullptr, 0, 0));
    }
    if (ok && !bytes.empty()) {
        ok = BCRYPT_SUCCESS(::BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
            static_cast<ULONG>(bytes.size()), 0));
    }
    if (ok) ok = BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(),
                                                    static_cast<ULONG>(digest.size()), 0));
    if (hash) ::BCryptDestroyHash(hash);
    if (algorithm) ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

} // namespace

SignatureStatus VerifyEcdsaP256(std::string_view bytes, std::string_view signatureBase64,
                               const unsigned char x[32], const unsigned char y[32]) {
    std::vector<unsigned char> signature;
    if (!DecodeBase64(signatureBase64, signature) || signature.size() != 64)
        return SignatureStatus::InvalidEncoding;
    std::array<unsigned char, 32> digest{};
    if (!Sha256(bytes, digest)) return SignatureStatus::CryptoFailure;

    struct PublicBlob {
        BCRYPT_ECCKEY_BLOB header;
        unsigned char x[32];
        unsigned char y[32];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::memcpy(blob.x, x, sizeof(blob.x));
    std::memcpy(blob.y, y, sizeof(blob.y));

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                                     nullptr, 0);
    if (BCRYPT_SUCCESS(status)) {
        status = ::BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                       reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    }
    if (BCRYPT_SUCCESS(status)) {
        status = ::BCryptVerifySignature(key, nullptr, digest.data(),
                                         static_cast<ULONG>(digest.size()), signature.data(),
                                         static_cast<ULONG>(signature.size()), 0);
    }
    if (key) ::BCryptDestroyKey(key);
    if (algorithm) ::BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status == kStatusSuccess) return SignatureStatus::Verified;
    if (status == kStatusInvalidSignature) return SignatureStatus::InvalidSignature;
    return SignatureStatus::CryptoFailure;
}

SignatureStatus VerifyManifestSignature(std::string_view manifestBytes,
                                        std::string_view signatureBase64,
                                        std::string_view keyId) {
    for (int i = 0; i < kTrustedUpdateKeyCount; ++i) {
        if (keyId == kTrustedUpdateKeys[i].keyId)
            return VerifyEcdsaP256(manifestBytes, signatureBase64,
                                   kTrustedUpdateKeys[i].x, kTrustedUpdateKeys[i].y);
    }
    return SignatureStatus::UnknownKey;
}

} // namespace cheburnet::update
