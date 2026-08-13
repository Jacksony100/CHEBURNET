#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cheburnet::update {

namespace detail {
using BodySink = std::function<bool(const void*, std::size_t)>;
using ResponseHook = std::function<bool(unsigned long, unsigned long long)>;
}

enum class HttpStatus {
    Ok, NotModified, Cancelled, InvalidUrl, InsecureRedirect, RedirectLimit,
    Timeout, TooLarge, Truncated, HttpError, WinHttpError
};

struct HttpOptions {
    std::uint64_t maxBytes = 64 * 1024;
    int maxRedirects = 4;
    int resolveTimeoutMs = 5000;
    int connectTimeoutMs = 8000;
    int sendTimeoutMs = 10000;
    int receiveTimeoutMs = 15000;
    std::wstring etag;
    std::atomic<bool>* cancel = nullptr;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
    // Internal sink used by DownloadToProtectedFile. Public callers normally
    // leave it empty and receive `body`.
    detail::BodySink bodySink;
    // Called after HTTP status/content-length are known and before body bytes.
    // Used by the protected streaming wrapper to reject size mismatches before
    // creating a privileged staging file.
    detail::ResponseHook responseHook;
};

struct HttpResult {
    HttpStatus status = HttpStatus::WinHttpError;
    unsigned long win32Error = 0;
    unsigned long httpStatus = 0;
    std::wstring etag;
    std::vector<unsigned char> body;
};

class WinHttpClient {
public:
    HttpResult Get(std::wstring_view url, const HttpOptions& options) const;

    // True streaming into SecureFs random CREATE_NEW staging with exact
    // size/hash/ACL/flush/atomic-rename validation.
    HttpResult DownloadToProtectedFile(std::wstring_view url, const HttpOptions& options,
                                       const std::wstring& outputPath,
                                       std::uint64_t expectedSize,
                                       std::string_view expectedSha256) const;
};

// Pure redirect policy used by the client and unit tests. Relative redirects
// and HTTPS-to-HTTP downgrades are rejected deliberately.
bool IsAllowedRedirect(std::wstring_view location);

// Deterministic WinHTTP error classification used by the client and tests.
HttpStatus ClassifyWinHttpError(unsigned long error);

} // namespace cheburnet::update
