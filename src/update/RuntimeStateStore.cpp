#include "RuntimeStateStore.h"

#include <windows.h>

#include "Version.h"
#include "../core/SecureFs.h"
#include "../util/Json.h"
#include "../util/StringUtil.h"

namespace cheburnet::update {
namespace {

const std::string* Text(const json::Value& object, std::string_view key) {
    const json::Value* value = object.Find(key);
    return value ? value->AsString() : nullptr;
}

bool ValidOptionalVersion(const std::string& version) {
    return version.empty() || ParseVersion(version).has_value();
}

} // namespace

StateResult LoadRuntimeState(const std::wstring& path) {
    StateResult result;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        result.missing = ::GetLastError() == ERROR_FILE_NOT_FOUND ||
                         ::GetLastError() == ERROR_PATH_NOT_FOUND;
        result.error = result.missing ? "state missing" : "cannot query state";
        return result;
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos ||
        !securefs::ValidatePathComponents(path.substr(0, slash)).ok ||
        !securefs::ValidateProtectedObject(path, securefs::ObjectKind::File, true).ok) {
        result.error = "unsafe active-runtime state object";
        return result;
    }
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.error = "cannot open state"; return result; }
    std::string bytes;
    char buffer[4096];
    DWORD read = 0;
    bool ioOk = true;
    for (;;) {
        if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            ioOk = false;
            break;
        }
        if (read == 0) break;
        bytes.append(buffer, read);
        if (bytes.size() > 64 * 1024) break;
    }
    ::CloseHandle(file);
    if (!ioOk) { result.error = "state read failed"; return result; }
    if (bytes.size() > 64 * 1024) { result.error = "state too large"; return result; }
    json::ParseOptions options;
    options.maxBytes = 64 * 1024;
    options.maxDepth = 8;
    options.maxValues = 64;
    const auto parsed = json::Parse(bytes, options);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    if (!root || root->size() != 7) { result.error = "malformed state"; return result; }
    const json::Value* schemaValue = parsed.value.Find("schema");
    const json::Number* schemaNumber = schemaValue ? schemaValue->AsNumber() : nullptr;
    const auto schema = schemaNumber ? schemaNumber->AsUInt64() : std::nullopt;
    const std::string* current = Text(parsed.value, "current");
    if (!schema || *schema != 1 || !current || !ParseVersion(*current)) {
        result.error = "unsupported state schema/version";
        return result;
    }
    result.state.current = *current;
    for (const auto [key, target] : {
             std::pair<std::string_view, std::string*>("previous_known_good",
                                                       &result.state.previousKnownGood),
             {"pending", &result.state.pending}, {"package_sha256", &result.state.packageSha256},
             {"manifest_key_id", &result.state.manifestKeyId},
             {"last_result", &result.state.lastResult}}) {
        const json::Value* value = parsed.value.Find(key);
        if (!value || value->IsNull()) continue;
        const std::string* text = value->AsString();
        if (!text) { result.error = "invalid state field type"; return result; }
        *target = *text;
    }
    const auto validSha = [](const std::string& value) {
        if (value.empty()) return true;
        if (value.size() != 64) return false;
        for (const char c : value)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    };
    const auto safeText = [](const std::string& value, std::size_t limit) {
        if (value.size() > limit) return false;
        for (const unsigned char c : value) if (c < 0x20u || c == 0x7fu) return false;
        return true;
    };
    if (!ValidOptionalVersion(result.state.previousKnownGood) ||
        !ValidOptionalVersion(result.state.pending) ||
        !validSha(result.state.packageSha256) ||
        !safeText(result.state.manifestKeyId, 64) ||
        !safeText(result.state.lastResult, 128)) {
        result.error = "invalid state field";
        return result;
    }
    result.ok = true;
    return result;
}

std::string SerializeRuntimeState(const StoredRuntimeState& state) {
    auto optional = [](const std::string& value) {
        return value.empty() ? std::string("null") : json::Quote(value);
    };
    std::string output = "{\n  \"schema\": 1,\n  \"current\": " + json::Quote(state.current) +
                         ",\n  \"previous_known_good\": " + optional(state.previousKnownGood) +
                         ",\n  \"pending\": " + optional(state.pending) +
                         ",\n  \"package_sha256\": " + optional(state.packageSha256) +
                         ",\n  \"manifest_key_id\": " + optional(state.manifestKeyId) +
                         ",\n  \"last_result\": " + optional(state.lastResult) + "\n}\n";
    return output;
}

bool SaveRuntimeState(const std::wstring& path, const StoredRuntimeState& state,
                      std::wstring* error) {
    const auto safeText = [](const std::string& value, std::size_t limit) {
        if (value.size() > limit) return false;
        for (const unsigned char c : value) if (c < 0x20u || c == 0x7fu) return false;
        return true;
    };
    if (!ParseVersion(state.current) || !ValidOptionalVersion(state.previousKnownGood) ||
        !ValidOptionalVersion(state.pending) ||
        (!state.packageSha256.empty() &&
         (state.packageSha256.size() != 64 ||
          state.packageSha256.find_first_not_of("0123456789abcdef") != std::string::npos)) ||
        !safeText(state.manifestKeyId, 64) || !safeText(state.lastResult, 128)) {
        if (error) *error = L"invalid runtime state version";
        return false;
    }
    const auto written = securefs::AtomicWrite(path, SerializeRuntimeState(state));
    if (!written.ok && error) *error = written.detail;
    return written.ok;
}

} // namespace cheburnet::update
