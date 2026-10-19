#include "WinHttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include "../util/Version.h"
#include "../core/SecureFs.h"

namespace cheburnet::update {

bool IsAllowedRedirect(std::wstring_view location) {
    return location.size() <= 2048 && location.rfind(L"https://", 0) == 0 &&
           location.find(L'@', 8) == std::wstring_view::npos &&
           location.find_first_of(L"\r\n\t\\") == std::wstring_view::npos;
}

namespace {

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { if (value_) ::WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value_; }
private:
    HINTERNET value_ = nullptr;
};

bool Cancelled(const HttpOptions& options) {
    return options.cancel && options.cancel->load(std::memory_order_relaxed);
}

// Only asynchronous WinHTTP requests are closed to cancel in-flight I/O.
// The callback context and read buffer remain alive through HANDLE_CLOSING,
// WinHTTP's final callback. No thread closes a synchronous pending handle.
class AsyncRequest final {
public:
    AsyncRequest(HINTERNET handle, const HttpOptions& options,
                 std::chrono::steady_clock::time_point deadline)
        : handle_(handle), options_(options), deadline_(deadline) {
        if (!handle_) return;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!::WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE,
                                &context, sizeof(context))) return;
        ready_ = ::WinHttpSetStatusCallback(handle_, Callback,
                     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                     0) != WINHTTP_INVALID_STATUS_CALLBACK;
    }
    ~AsyncRequest() { Close(); }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;
    HINTERNET get() const { return handle_; }
    bool ready() const { return ready_; }

    template<class Begin>
    bool Run(DWORD expected, Begin begin, DWORD* read = nullptr) {
        {
            std::scoped_lock lock(mutex_);
            completed_ = 0;
            error_ = 0;
            bytes_ = 0;
        }
        if (Cancelled(options_)) { ::SetLastError(ERROR_OPERATION_ABORTED); return false; }
        if (std::chrono::steady_clock::now() >= deadline_) {
            ::SetLastError(ERROR_WINHTTP_TIMEOUT); return false;
        }
        if (!begin() && ::GetLastError() != ERROR_IO_PENDING) return false;
        std::unique_lock lock(mutex_);
        while (completed_ != expected && error_ == 0) {
            const bool cancel = Cancelled(options_);
            const bool timeout = std::chrono::steady_clock::now() >= deadline_;
            if (cancel || timeout) {
                lock.unlock();
                Close();
                ::SetLastError(cancel ? ERROR_OPERATION_ABORTED : ERROR_WINHTTP_TIMEOUT);
                return false;
            }
            condition_.wait_for(lock, std::chrono::milliseconds(25));
        }
        if (error_ != 0) { ::SetLastError(error_); return false; }
        if (read) *read = bytes_;
        return true;
    }

private:
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status,
                                   void* information, DWORD length) noexcept {
        if (!context) return;
        auto& self = *reinterpret_cast<AsyncRequest*>(context);
        // Notifications are serialized by the mutex; Close waits until the
        // final notification has released it before destroying this object.
        std::scoped_lock lock(self.mutex_);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self.closed_ = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && information)
            self.error_ = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
                 status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                 status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            self.completed_ = status;
            if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self.bytes_ = length;
        }
        self.condition_.notify_all();
    }
    void Close() {
        const HINTERNET handle = std::exchange(handle_, nullptr);
        if (!handle) return;
        ::WinHttpCloseHandle(handle);
        if (ready_) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return closed_; });
        }
    }
    HINTERNET handle_ = nullptr;
    const HttpOptions& options_;
    std::chrono::steady_clock::time_point deadline_;
    std::mutex mutex_;
    std::condition_variable condition_;
    DWORD completed_ = 0;
    DWORD error_ = 0;
    DWORD bytes_ = 0;
    bool closed_ = false;
    bool ready_ = false;
};

} // namespace

HttpStatus ClassifyWinHttpError(unsigned long error) {
    if (error == ERROR_OPERATION_ABORTED || error == ERROR_WINHTTP_OPERATION_CANCELLED)
        return HttpStatus::Cancelled;
    return error == ERROR_WINHTTP_TIMEOUT ? HttpStatus::Timeout : HttpStatus::WinHttpError;
}

HttpResult WinHttpClient::Get(std::wstring_view initialUrl, const HttpOptions& options) const {
    HttpResult result;
    if (initialUrl.size() > 2048 || initialUrl.rfind(L"https://", 0) != 0) {
        result.status = HttpStatus::InvalidUrl;
        return result;
    }
    if (Cancelled(options)) { result.status = HttpStatus::Cancelled; return result; }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds((std::max)(1, options.totalTimeoutMs));
    const std::wstring agent = L"CHEBURNET/" CHEBURNET_VERSION_WSTR;
    InternetHandle session(::WinHttpOpen(agent.c_str(), options.useSystemProxy
                                         ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                         : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                         WINHTTP_FLAG_ASYNC));
    if (!session.get()) {
        result.win32Error = ::GetLastError();
        result.status = ClassifyWinHttpError(result.win32Error);
        return result;
    }
    ::WinHttpSetTimeouts(session.get(), options.resolveTimeoutMs, options.connectTimeoutMs,
                         options.sendTimeoutMs, options.receiveTimeoutMs);

    std::wstring current(initialUrl);
    for (int redirect = 0; redirect <= options.maxRedirects; ++redirect) {
        if (Cancelled(options)) { result.status = HttpStatus::Cancelled; return result; }
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!::WinHttpCrackUrl(current.c_str(), static_cast<DWORD>(current.size()), 0, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS) {
            result.status = HttpStatus::InvalidUrl;
            return result;
        }
        std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        InternetHandle connection(::WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (!connection.get()) {
            result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
        }
        std::vector<unsigned char> buffer(64 * 1024, 0);
        AsyncRequest request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE), options, deadline);
        if (!request.get() || !request.ready()) {
            result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                                &redirectPolicy, sizeof(redirectPolicy))) {
            result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
        }
        std::wstring headers;
        if (!options.etag.empty()) headers = L"If-None-Match: " + options.etag + L"\r\n";
        if (!request.Run(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&] {
                return ::WinHttpSendRequest(request.get(),
                    headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                    headers.empty() ? 0 : static_cast<DWORD>(-1),
                    WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(&request));
            }) ||
            !request.Run(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
                         [&] { return ::WinHttpReceiveResponse(request.get(), nullptr); })) {
            result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
        }
        DWORD status = 0, statusSize = sizeof(status);
        if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                   WINHTTP_NO_HEADER_INDEX)) {
            result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
        }
        result.httpStatus = status;
        DWORD etagBytes = 0;
        if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_ETAG,
                                   WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &etagBytes,
                                   WINHTTP_NO_HEADER_INDEX) &&
            ::GetLastError() == ERROR_INSUFFICIENT_BUFFER && etagBytes >= sizeof(wchar_t)) {
            std::wstring etag(etagBytes / sizeof(wchar_t), L'\0');
            if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_ETAG,
                                      WINHTTP_HEADER_NAME_BY_INDEX, etag.data(), &etagBytes,
                                      WINHTTP_NO_HEADER_INDEX)) {
                etag.resize((etagBytes / sizeof(wchar_t)) - 1);
                if (etag.size() <= 512 && etag.find_first_of(L"\r\n") == std::wstring::npos)
                    result.etag = std::move(etag);
            }
        }
        if (status == HTTP_STATUS_NOT_MODIFIED) { result.status = HttpStatus::NotModified; return result; }
        if (status >= 300 && status < 400) {
            if (redirect == options.maxRedirects) { result.status = HttpStatus::RedirectLimit; return result; }
            DWORD bytes = 0;
            ::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                                  WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
                                  WINHTTP_NO_HEADER_INDEX);
            if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) {
                result.status = HttpStatus::HttpError; return result;
            }
            std::wstring location(bytes / sizeof(wchar_t), L'\0');
            if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                                       WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &bytes,
                                       WINHTTP_NO_HEADER_INDEX)) {
                result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
            }
            location.resize((bytes / sizeof(wchar_t)) - 1);
            // Reject relative and HTTPS->HTTP redirects; the signed manifest
            // publisher must provide explicit HTTPS locations.
            if (!IsAllowedRedirect(location)) {
                result.status = HttpStatus::InsecureRedirect; return result;
            }
            current = std::move(location);
            continue;
        }
        if (status != HTTP_STATUS_OK) { result.status = HttpStatus::HttpError; return result; }

        unsigned long long declared = 0;
        DWORD declaredSize = sizeof(declared);
        if (::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH |
                                                    WINHTTP_QUERY_FLAG_NUMBER64,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &declared, &declaredSize,
                                  WINHTTP_NO_HEADER_INDEX) && declared > options.maxBytes) {
            result.status = HttpStatus::TooLarge; return result;
        }
        if (options.responseHook && !options.responseHook(status, declared)) {
            result.status = HttpStatus::Truncated;
            return result;
        }
        result.body.clear();
        // Heap, not stack: a 64 KiB automatic buffer put ~66 KB on a single
        // frame (MSVC /analyze C6262). This function also runs on the
        // background update-check thread, so the transfer buffer must not
        // depend on how much stack that thread happens to have.
        std::uint64_t total = 0;
        for (;;) {
            if (Cancelled(options)) { result.status = HttpStatus::Cancelled; return result; }
            DWORD read = 0;
            if (!request.Run(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&] {
                    return ::WinHttpReadData(request.get(), buffer.data(),
                                             static_cast<DWORD>(buffer.size()), nullptr);
                }, &read)) {
                result.win32Error = ::GetLastError(); result.status = ClassifyWinHttpError(result.win32Error); return result;
            }
            if (read == 0) break;
            if (read > options.maxBytes || total > options.maxBytes - read) {
                result.status = HttpStatus::TooLarge; return result;
            }
            total += read;
            if (!options.bodySink)
                result.body.insert(result.body.end(), buffer.begin(), buffer.begin() + read);
            else if (!options.bodySink(buffer.data(), read)) {
                result.status = Cancelled(options) ? HttpStatus::Cancelled : HttpStatus::Truncated;
                return result;
            }
            if (options.progress) options.progress(total, declared);
        }
        if (declared != 0 && total != declared) {
            result.status = HttpStatus::Truncated; return result;
        }
        result.status = HttpStatus::Ok;
        return result;
    }
    result.status = HttpStatus::RedirectLimit;
    return result;
}

HttpResult WinHttpClient::DownloadToProtectedFile(std::wstring_view url,
                                                  const HttpOptions& options,
                                                  const std::wstring& outputPath,
                                                  std::uint64_t expectedSize,
                                                  std::string_view expectedSha256) const {
    HttpResult network;
    if (expectedSize == 0 || options.maxBytes != expectedSize || expectedSha256.size() != 64) {
        network.status = HttpStatus::InvalidUrl;
        return network;
    }
    HttpOptions streaming = options;
    streaming.responseHook = [&](unsigned long, unsigned long long declared) {
        return declared == 0 || declared == expectedSize;
    };
    streaming.bodySink = [&](const void*, std::size_t) { return false; };
    const securefs::Result write = securefs::AtomicWriteStream(
        outputPath, expectedSize, std::string(expectedSha256),
        [&](const securefs::StreamSink& sink) {
            streaming.bodySink = sink;
            network = Get(url, streaming);
            return network.status == HttpStatus::Ok;
        });
    if (!write.ok && network.status == HttpStatus::Ok) network.status = HttpStatus::Truncated;
    return network;
}

} // namespace cheburnet::update
