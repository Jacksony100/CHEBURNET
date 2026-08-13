#include "IntegrityVerifier.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

#include "../util/StringUtil.h"

#pragma comment(lib, "bcrypt.lib")

namespace cheburnet {
namespace {

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

std::string ToHex(const unsigned char* bytes, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[bytes[i] & 0xF];
    }
    return out;
}

// RAII around a BCrypt hash session for SHA-256.
class Sha256Session {
public:
    bool Init() {
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            return false;
        DWORD cb = 0, objLen = 0;
        if (!BCRYPT_SUCCESS(::BCryptGetProperty(alg_, BCRYPT_OBJECT_LENGTH,
                                                reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen),
                                                &cb, 0)))
            return false;
        if (!BCRYPT_SUCCESS(::BCryptGetProperty(alg_, BCRYPT_HASH_LENGTH,
                                                reinterpret_cast<PUCHAR>(&hashLen_), sizeof(hashLen_),
                                                &cb, 0)))
            return false;
        obj_.resize(objLen);
        if (!BCRYPT_SUCCESS(::BCryptCreateHash(alg_, &hash_, obj_.data(),
                                               static_cast<ULONG>(obj_.size()), nullptr, 0, 0)))
            return false;
        return true;
    }

    bool Update(const void* data, size_t len) {
        return BCRYPT_SUCCESS(::BCryptHashData(
            hash_, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(len), 0));
    }

    std::optional<std::string> Finish() {
        std::vector<unsigned char> digest(hashLen_);
        if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash_, digest.data(), hashLen_, 0)))
            return std::nullopt;
        return ToHex(digest.data(), digest.size());
    }

    ~Sha256Session() {
        if (hash_) ::BCryptDestroyHash(hash_);
        if (alg_) ::BCryptCloseAlgorithmProvider(alg_, 0);
    }

private:
    BCRYPT_ALG_HANDLE  alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> obj_;
    DWORD hashLen_ = 0;
};

} // namespace

std::optional<std::string> IntegrityVerifier::Sha256Hex(const void* data, size_t len) {
    Sha256Session s;
    if (!s.Init()) return std::nullopt;
    if (len > 0 && !s.Update(data, len)) return std::nullopt;
    return s.Finish();
}

std::optional<std::string> IntegrityVerifier::Sha256File(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) {
        ::CloseHandle(h);
        return std::nullopt;
    }

    Sha256Session s;
    if (!s.Init()) {
        ::CloseHandle(h);
        return std::nullopt;
    }

    std::vector<unsigned char> buf(64 * 1024);
    DWORD read = 0;
    bool ok = true;
    for (;;) {
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        if (!s.Update(buf.data(), read)) {
            ok = false;
            break;
        }
    }
    ::CloseHandle(h);
    if (!ok) return std::nullopt;
    return s.Finish();
}

bool IntegrityVerifier::HexEquals(const std::string& a, const std::string& b) {
    return str::IEqualsAscii(a, b);
}

std::optional<unsigned long long> IntegrityVerifier::FileSize(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) {
        ::CloseHandle(file);
        return std::nullopt;
    }
    ::CloseHandle(file);
    ULARGE_INTEGER li;
    li.HighPart = info.nFileSizeHigh;
    li.LowPart = info.nFileSizeLow;
    return li.QuadPart;
}

} // namespace cheburnet
