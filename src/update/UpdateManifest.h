#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cheburnet::update {

enum class ArtifactKind { Launcher, Payload };

struct Artifact {
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
    std::string minimumSupportedVersion;
    std::string minimumLauncherVersion;
    int payloadSchema = 0;
    int strategySchema = 0;
    std::string provider;
    std::string upstreamReleaseUrl;
};

struct Manifest {
    int schema = 0;
    std::string channel;
    std::string generatedAt;
    std::string keyId;
    Artifact launcher;
    Artifact payload;
};

struct ManifestResult {
    bool ok = false;
    Manifest manifest;
    std::string error;
};

ManifestResult ParseManifest(std::string_view bytes, std::size_t maxBytes = 64 * 1024);
bool IsHttpsUrl(std::string_view value);

enum class Eligibility { Current, Upgrade, DowngradeRejected, PrereleaseRejected,
                         LauncherTooOld, SchemaUnsupported, InvalidVersion };
Eligibility EvaluatePayload(const Manifest& manifest, std::string_view currentPayload,
                            std::string_view launcherVersion, bool stableChannel);
Eligibility EvaluateLauncher(const Manifest& manifest, std::string_view currentLauncher,
                             bool stableChannel);

} // namespace cheburnet::update
