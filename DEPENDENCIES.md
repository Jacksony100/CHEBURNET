# Dependency summary

CHEBURNET launcher code uses only the C++20 standard library and Windows SDK
APIs (Win32, CNG/BCrypt, Crypt32, WinHTTP, ACL, Shell and console APIs). It links
the static MSVC runtime and adds no third-party source library dependency.

Embedded runtime components and their source/license locations are enumerated
in `THIRD_PARTY_NOTICES.md` and `LICENSES/`. Exact upstream release provenance is
stored in `resources/upstream/provenance.json` and embedded into the executable.
