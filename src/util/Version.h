#pragma once

// Generated from the CMake project() version. The same macros also feed the PE
// version resource; tag releases independently enforce an exact version match.
#include "GeneratedVersion.h"

// Update compatibility schemas. Payload packages with newer schemas are
// rejected until the launcher itself is upgraded.
#define CHEBURNET_PAYLOAD_SCHEMA 1
#define CHEBURNET_STRATEGY_SCHEMA 1

#define CHEBURNET_PRODUCT_NAME "CHEBURNET"
#define CHEBURNET_PRODUCT_WNAME L"CHEBURNET"
#define CHEBURNET_WINDOW_TITLE L"CHEBURNET // CONNECTION CONSOLE"
