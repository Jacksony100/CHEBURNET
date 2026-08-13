#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cheburnet::securefs {

enum class ObjectKind { File, Directory };

struct Result {
    bool          ok = false;
    unsigned long win32Error = 0;
    std::wstring  detail;
};

// Rejects missing/wrong-type/reparse objects and opens the object itself rather
// than following it. For files, optionally requires a single hard link.
Result ValidateObject(const std::wstring& path, ObjectKind kind, bool requireSingleLink = true);

// Every existing component from the volume root through `path` must be a real
// directory (never junction/symlink/mount reparse). Missing components fail.
Result ValidatePathComponents(const std::wstring& path);

// Validate object type plus the exact CHEBURNET owner/protected-DACL policy
// without changing it. Use this before trusting existing state or records:
// hardening an attacker-preplanted file does not authenticate its contents.
Result ValidateProtectedObject(const std::wstring& path, ObjectKind kind,
                               bool requireSingleLink = true);

// Validate the object and exact protected ACL through an already-open
// no-follow handle. The caller must request FILE_READ_ATTRIBUTES and
// READ_CONTROL when opening the handle. This avoids reopening a live log or
// redirected output file by name, so sharing conflicts and name-swap races do
// not weaken the check.
Result ValidateProtectedHandle(void* handle, const std::wstring& path,
                               ObjectKind kind, bool requireSingleLink = true);

// Administrators owner; protected SYSTEM/Administrators full, Users read/execute.
Result HardenObject(const std::wstring& path, ObjectKind kind);

// Create the directory chain one component at a time without ever following a
// reparse point, then harden and verify every component below the volume root.
Result EnsureProtectedDirectory(const std::wstring& path);

// Privileged atomic write: 128-bit random temp, CREATE_NEW + OPEN_REPARSE_POINT,
// exact write/flush, mandatory ACL, SHA-256, atomic replace, reopen verification.
Result AtomicWrite(const std::wstring& target, const void* data, std::size_t size,
                   const std::string& expectedSha256 = {});

// Save a protected UTF-8/text record using AtomicWrite.
Result AtomicWrite(const std::wstring& target, const std::string& data,
                   const std::string& expectedSha256 = {});

using StreamSink = std::function<bool(const void*, std::size_t)>;
using StreamProducer = std::function<bool(const StreamSink&)>;
Result AtomicWriteStream(const std::wstring& target, std::uint64_t expectedSize,
                         const std::string& expectedSha256,
                         const StreamProducer& producer);

// Strict descendant test used before recursive cleanup/package deletion.
bool IsStrictDescendant(const std::wstring& root, const std::wstring& candidate);

// No-reparse recursive delete; refuses roots, parents and non-descendants.
Result RemoveTreeUnder(const std::wstring& allowedRoot, const std::wstring& target);

// Security bootstrap. No logger is called here: diagnostics are returned to the
// caller for stderr/console/MessageBox reporting.
Result BootstrapProtectedTree(const std::wstring& programDataRoot,
                              const std::wstring& logsDir,
                              const std::wstring& runtimeRoot,
                              const std::wstring& updatesDir,
                              const std::wstring& userDir);

} // namespace cheburnet::securefs
