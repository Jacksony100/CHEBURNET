#pragma once

// Generated from cmake/Version.cmake — the single authoritative version model.
//
// CHEBURNET_VERSION_STR is the SEMANTIC version ("1.0.0-rc.3" / "1.0.0") and is
// the only value update eligibility is ever evaluated against.
// CHEBURNET_VERSION_PE_STR / CHEBURNET_VERSION_QUAD carry the numeric-only PE
// representation ("1.0.0.3" / "1.0.0.0"); PE format cannot express a prerelease
// and must never be used for upgrade decisions.
// CHEBURNET_STABLE_CHANNEL is 1 for a stable build and 0 for an RC build.
#include "GeneratedVersion.h"

// Update compatibility schemas. Payload packages with newer schemas are
// rejected until the launcher itself is upgraded.
#define CHEBURNET_PAYLOAD_SCHEMA 1
#define CHEBURNET_STRATEGY_SCHEMA 1

#define CHEBURNET_PRODUCT_NAME "CHEBURNET"
#define CHEBURNET_PRODUCT_WNAME L"CHEBURNET"
#define CHEBURNET_WINDOW_TITLE L"CHEBURNET // УПРАВЛЕНИЕ СОЕДИНЕНИЕМ"
