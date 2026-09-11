#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cheburnet::update {

// Каноническая грамматика версии (единственная для лончера и для движка):
//
//   version    := ['v'] core [revision] [prerelease] [build]
//   core       := NUM ('.' NUM){0,7}          NUM без ведущих нулей
//   revision   := [a-z]                       ревизия upstream (1.10.1a)
//   prerelease := '-' ident ('.' ident)*       ident := [0-9A-Za-z-]+
//   build      := '+' ident ('.' ident)*
//
// Порядок строго определён, без псевдонимов — от него зависит защита от
// понижения версии:
//   1. core сравнивается численно с дополнением нулями;
//   2. отсутствие revision < наличие revision, далее лексикографически,
//      поэтому 1.10.1 < 1.10.1a (ревизия upstream новее базового выпуска);
//   3. наличие prerelease < отсутствие prerelease, далее по правилам
//      precedence SemVer 2.0.0 (числовой идентификатор сравнивается численно
//      и всегда младше алфавитно-численного; более короткий набор младше);
//   4. build-метаданные на порядок не влияют.
struct Version {
    std::vector<unsigned long long> parts;   // core
    std::string suffix;                      // всё после core, как в исходной строке
    bool prerelease = false;                 // присутствует SemVer prerelease
    std::string revision;                    // ревизия upstream ("a") либо пусто
    std::vector<std::string> prereleaseIds;  // идентификаторы prerelease, без build
};

std::optional<Version> ParseVersion(std::string_view value);
int CompareVersions(const Version& left, const Version& right);

} // namespace cheburnet::update
