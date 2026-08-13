#include "UpdateManifest.h"

#include "Version.h"
#include "../util/Json.h"
#include "../util/Version.h"

namespace cheburnet::update {
namespace {

const json::Value* Required(const json::Value& object, std::string_view key) {
    return object.Find(key);
}

bool ReadString(const json::Value& object, std::string_view key, std::string& output,
                bool allowEmpty = false) {
    const json::Value* value = Required(object, key);
    const std::string* text = value ? value->AsString() : nullptr;
    if (!text || (!allowEmpty && text->empty())) return false;
    output = *text;
    return true;
}

bool ReadUInt(const json::Value& object, std::string_view key, std::uint64_t& output) {
    const json::Value* value = Required(object, key);
    const json::Number* number = value ? value->AsNumber() : nullptr;
    const auto parsed = number ? number->AsUInt64() : std::nullopt;
    if (!parsed) return false;
    output = *parsed;
    return true;
}

bool ValidSha(std::string_view value) {
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool ValidGeneratedAt(std::string_view value) {
    // Signed publisher metadata is still parsed strictly so malformed release
    // automation output cannot become trusted state. UTC seconds precision is
    // the one canonical format emitted by generate-update-manifest.ps1.
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
                                11u, 12u, 14u, 15u, 17u, 18u}) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    const auto two = [&](std::size_t at) {
        return static_cast<unsigned>((value[at] - '0') * 10 + (value[at + 1] - '0'));
    };
    const unsigned month = two(5), day = two(8), hour = two(11);
    const unsigned minute = two(14), second = two(17);
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
           hour <= 23 && minute <= 59 && second <= 59;
}

bool ValidKeyId(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool ParseArtifact(const json::Value& value, Artifact& artifact, bool payload) {
    const json::Value::Object* object = value.AsObject();
    const std::size_t expectedMembers = payload ? 9u : 5u;
    if (!object || object->size() != expectedMembers ||
        !ReadString(value, "version", artifact.version) ||
        !ReadString(value, "url", artifact.url) || !ReadString(value, "sha256", artifact.sha256) ||
        !ReadUInt(value, "size", artifact.size) || artifact.size == 0 ||
        artifact.size > 512ull * 1024ull * 1024ull || !ValidSha(artifact.sha256) ||
        !IsHttpsUrl(artifact.url) || !ParseVersion(artifact.version)) return false;
    if (payload) {
        std::uint64_t payloadSchema = 0, strategySchema = 0;
        if (!ReadString(value, "provider", artifact.provider) ||
            !ReadString(value, "minimum_launcher_version", artifact.minimumLauncherVersion) ||
            !ReadString(value, "upstream_release_url", artifact.upstreamReleaseUrl) ||
            !ReadUInt(value, "payload_schema", payloadSchema) ||
            !ReadUInt(value, "strategy_schema", strategySchema) ||
            payloadSchema > 1000 || strategySchema > 1000 ||
            artifact.provider != "Flowseal/zapret-discord-youtube" ||
            artifact.size > 129ull * 1024ull * 1024ull ||
            !IsHttpsUrl(artifact.upstreamReleaseUrl))
            return false;
        artifact.payloadSchema = static_cast<int>(payloadSchema);
        artifact.strategySchema = static_cast<int>(strategySchema);
    } else if (!ReadString(value, "minimum_supported_version", artifact.minimumSupportedVersion) ||
               artifact.size > 64ull * 1024ull * 1024ull ||
               !ParseVersion(artifact.minimumSupportedVersion)) {
        return false;
    }
    return true;
}

} // namespace

bool IsHttpsUrl(std::string_view value) {
    if (value.size() < 9 || value.size() > 2048 || value.rfind("https://", 0) != 0) return false;
    if (value.find('@', 8) != std::string_view::npos || value.find('\\') != std::string_view::npos ||
        value.find_first_of("\r\n\t") != std::string_view::npos) return false;
    const std::size_t hostEnd = value.find('/', 8);
    const std::string_view host = value.substr(8, hostEnd == std::string_view::npos
                                                     ? std::string_view::npos : hostEnd - 8);
    return !host.empty() && host != "." && host != "..";
}

ManifestResult ParseManifest(std::string_view bytes, std::size_t maxBytes) {
    ManifestResult result;
    json::ParseOptions options;
    options.maxBytes = maxBytes;
    options.maxDepth = 12;
    options.maxValues = 256;
    const json::ParseResult parsed = json::Parse(bytes, options);
    const json::Value::Object* root = parsed.ok ? parsed.value.AsObject() : nullptr;
    if (!root || root->size() != 6) {
        result.error = parsed.ok ? "manifest root is not an object" : parsed.error;
        return result;
    }
    std::uint64_t schema = 0;
    if (!ReadUInt(parsed.value, "schema", schema) || schema != 1 ||
        !ReadString(parsed.value, "channel", result.manifest.channel) ||
        result.manifest.channel != "stable" ||
        !ReadString(parsed.value, "generated_at", result.manifest.generatedAt) ||
        !ReadString(parsed.value, "key_id", result.manifest.keyId) ||
        !ValidGeneratedAt(result.manifest.generatedAt) || !ValidKeyId(result.manifest.keyId)) {
        result.error = "unsupported or incomplete manifest envelope";
        return result;
    }
    const json::Value* launcher = parsed.value.Find("launcher");
    const json::Value* payload = parsed.value.Find("payload");
    if (!launcher || !payload || !ParseArtifact(*launcher, result.manifest.launcher, false) ||
        !ParseArtifact(*payload, result.manifest.payload, true)) {
        result.error = "invalid manifest artifact";
        return result;
    }
    result.manifest.schema = static_cast<int>(schema);
    result.ok = true;
    return result;
}

Eligibility EvaluatePayload(const Manifest& manifest, std::string_view currentPayload,
                            std::string_view launcherVersion, bool stableChannel) {
    const auto current = ParseVersion(currentPayload);
    const auto candidate = ParseVersion(manifest.payload.version);
    const auto launcher = ParseVersion(launcherVersion);
    const auto minimum = ParseVersion(manifest.payload.minimumLauncherVersion);
    if (!current || !candidate || !launcher || !minimum) return Eligibility::InvalidVersion;
    if (stableChannel && candidate->prerelease) return Eligibility::PrereleaseRejected;
    const int compared = CompareVersions(*candidate, *current);
    if (compared < 0) return Eligibility::DowngradeRejected;
    if (compared == 0) return Eligibility::Current;
    if (CompareVersions(*launcher, *minimum) < 0) return Eligibility::LauncherTooOld;
    if (manifest.payload.payloadSchema != CHEBURNET_PAYLOAD_SCHEMA ||
        manifest.payload.strategySchema != CHEBURNET_STRATEGY_SCHEMA)
        return Eligibility::SchemaUnsupported;
    return Eligibility::Upgrade;
}

Eligibility EvaluateLauncher(const Manifest& manifest, std::string_view currentLauncher,
                             bool stableChannel) {
    const auto current = ParseVersion(currentLauncher);
    const auto candidate = ParseVersion(manifest.launcher.version);
    const auto minimum = ParseVersion(manifest.launcher.minimumSupportedVersion);
    if (!current || !candidate || !minimum) return Eligibility::InvalidVersion;
    if (stableChannel && candidate->prerelease) return Eligibility::PrereleaseRejected;
    const int compared = CompareVersions(*candidate, *current);
    if (compared < 0) return Eligibility::DowngradeRejected;
    if (compared == 0) return Eligibility::Current;
    if (CompareVersions(*current, *minimum) < 0) return Eligibility::LauncherTooOld;
    return Eligibility::Upgrade;
}

} // namespace cheburnet::update
