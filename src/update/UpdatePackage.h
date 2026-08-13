#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cheburnet::update {

enum class PackageEntryType { Regular, Symlink, Reparse, Other };

struct PackageEntry {
    std::string path;
    PackageEntryType type = PackageEntryType::Regular;
    std::uint64_t size = 0;
    std::string sha256;
};

struct PackageLimits {
    std::size_t maxFiles = 256;
    std::uint64_t maxTotalBytes = 128ull * 1024ull * 1024ull;
    std::uint64_t maxFileBytes = 32ull * 1024ull * 1024ull;
};

struct PackageValidation {
    bool ok = false;
    std::string error;
};

struct PackageInfo {
    int schema = 0;
    std::string payloadVersion;
    std::string provider;
    int payloadSchema = 0;
    int strategySchema = 0;
    std::uint64_t dataOffset = 0;
    std::size_t runtimeManifestIndex = static_cast<std::size_t>(-1);
    std::vector<PackageEntry> entries;
};

struct PackageResult {
    bool ok = false;
    PackageInfo package;
    std::string error;
};

// CHEBURNET package paths use UTF-8 and '/' separators. Only bin/, lists/,
// strategies/, provenance.json and the generated runtime-manifest.json are
// allowed. No arbitrary upstream archive is interpreted by the client.
bool NormalizePackagePath(std::string_view input, std::string& normalized);
PackageValidation ValidatePackageEntries(const std::vector<PackageEntry>& entries,
                                         const PackageLimits& limits = {});

PackageResult ParsePackageFile(const std::wstring& path, const PackageLimits& limits = {});

// Size + SHA-256 post-download validation shared by updater and deterministic
// negative tests. Reparse points and hard links are rejected by the verifier.
bool ValidateArtifactFile(const std::wstring& path, std::uint64_t expectedSize,
                          std::string_view expectedSha256);

// Destination must be a new strict child of protectedRuntimeRoot. Extraction
// uses SecureFs atomic writes and removes the incomplete child on any failure.
PackageValidation ExtractPackage(const std::wstring& packagePath, const PackageInfo& package,
                                 const std::wstring& protectedRuntimeRoot,
                                 const std::wstring& destination);

// Verify every extracted entry and the generated runtime manifest once more.
PackageValidation VerifyExtractedPackage(const PackageInfo& package,
                                         const std::wstring& destination);

} // namespace cheburnet::update
