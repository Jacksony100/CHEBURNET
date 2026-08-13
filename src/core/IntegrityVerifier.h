#pragma once
#include <cstddef>
#include <optional>
#include <string>

namespace cheburnet {

// SHA-256 via the Windows CNG (BCrypt) API. Returns lowercase hex strings.
class IntegrityVerifier {
public:
    // Hash an in-memory buffer. Returns nullopt on CNG failure.
    static std::optional<std::string> Sha256Hex(const void* data, size_t len);

    // Hash a file by streaming it in blocks. Returns nullopt if the file cannot
    // be opened or CNG fails.
    static std::optional<std::string> Sha256File(const std::wstring& path);

    // Constant-time-ish case-insensitive hex comparison.
    static bool HexEquals(const std::string& a, const std::string& b);

    // Size of a file, or nullopt if it cannot be queried.
    static std::optional<unsigned long long> FileSize(const std::wstring& path);
};

} // namespace cheburnet
