cmake_minimum_required(VERSION 3.25)

# Единственный авторитетный источник версии (семантическая + числовая PE +
# prerelease + канал). Подключается до project(), поэтому использует
# CMAKE_CURRENT_LIST_DIR.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Version.cmake")

project(CHEBURNET VERSION "${CHEBURNET_VERSION_PE}" LANGUAGES CXX)

message(STATUS "CHEBURNET: версия ${CHEBURNET_VERSION_SEMANTIC} "
               "(PE ${CHEBURNET_VERSION_PE}, канал ${CHEBURNET_RELEASE_CHANNEL})")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "CHEBURNET builds with MSVC (Windows x64) only.")
endif()

# Static MSVC runtime (/MT) so the release needs no VC++ redistributable.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Code generation (single source of truth for resources & strategies).
# ---------------------------------------------------------------------------
find_program(POWERSHELL_EXE NAMES powershell.exe powershell REQUIRED)

set(GEN_DIR       "${CMAKE_SOURCE_DIR}/resources/generated")
set(PAYLOAD_DIR   "${CMAKE_SOURCE_DIR}/resources/payload")
set(STRATEGY_SRC  "${CMAKE_SOURCE_DIR}/resources/strategies_src")

file(MAKE_DIRECTORY "${GEN_DIR}")

# Launcher version is a single source of truth. C++, PE VERSIONINFO and the
# embedded application manifest are all configured from cmake/Version.cmake.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/GeneratedVersion.h.in"
    "${GEN_DIR}/GeneratedVersion.h" @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/resources/app.manifest.in"
    "${GEN_DIR}/app.manifest" @ONLY)

message(STATUS "CHEBURNET: generating resource manifest + payload .rc")
execute_process(
    COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/gen_resources.ps1"
            -PayloadDir "${PAYLOAD_DIR}"
            -RcOut "${GEN_DIR}/cheburnet_payload.rc"
            -HeaderOut "${GEN_DIR}/GeneratedManifest.h"
    RESULT_VARIABLE _gen_res_rc)
if(NOT _gen_res_rc EQUAL 0)
    message(FATAL_ERROR "gen_resources.ps1 failed (${_gen_res_rc})")
endif()

message(STATUS "CHEBURNET: generating strategy table")
execute_process(
    COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/gen_strategies.ps1"
            -BatDir "${STRATEGY_SRC}"
            -HeaderOut "${GEN_DIR}/GeneratedStrategies.h"
            -CatalogOut "${GEN_DIR}/strategy-catalog.json"
    RESULT_VARIABLE _gen_strat_rc)
if(NOT _gen_strat_rc EQUAL 0)
    message(FATAL_ERROR "gen_strategies.ps1 failed (${_gen_strat_rc})")
endif()

message(STATUS "CHEBURNET: generating mascot art")
execute_process(
    COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/gen_mascot.ps1"
            -ArtFile "${CMAKE_SOURCE_DIR}/resources/mascot/cheburashka.txt"
            -HeaderOut "${GEN_DIR}/MascotFullArt.h"
    RESULT_VARIABLE _gen_mascot_rc)
if(NOT _gen_mascot_rc EQUAL 0)
    message(FATAL_ERROR "gen_mascot.ps1 failed (${_gen_mascot_rc})")
endif()

message(STATUS "CHEBURNET: validating upstream provenance")
execute_process(
    COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/gen_provenance.ps1"
            -Provenance "${CMAKE_SOURCE_DIR}/resources/upstream/provenance.json"
            -HeaderOut "${GEN_DIR}/GeneratedProvenance.h"
    RESULT_VARIABLE _gen_provenance_rc)
if(NOT _gen_provenance_rc EQUAL 0)
    message(FATAL_ERROR "gen_provenance.ps1 failed (${_gen_provenance_rc})")
endif()

message(STATUS "CHEBURNET: generating update trust store")
execute_process(
    COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/cmake/gen_update_keys.ps1"
            -PublicKey "${CMAKE_SOURCE_DIR}/resources/update/release-public-key.json"
            -HeaderOut "${GEN_DIR}/GeneratedUpdateKeys.h"
    RESULT_VARIABLE _gen_keys_rc)
if(NOT _gen_keys_rc EQUAL 0)
    message(FATAL_ERROR "gen_update_keys.ps1 failed (${_gen_keys_rc})")
endif()

# ---------------------------------------------------------------------------
# Shared logic library (used by the launcher and the tests).
# ---------------------------------------------------------------------------
set(LIB_SOURCES
    src/util/StringUtil.cpp
    src/util/Win32Error.cpp
    src/util/Logger.cpp
    src/util/CommandLine.cpp
    src/util/Json.cpp
    src/config/Config.cpp
    src/config/ProgressModel.cpp
    src/config/Strategies.cpp
    src/core/RuntimePaths.cpp
    src/core/SecureFs.cpp
    src/core/IntegrityVerifier.cpp
    src/core/PrivilegeManager.cpp
    src/core/ResourceExtractor.cpp
    src/core/StatusProbe.cpp
    src/core/ProcessManager.cpp
    src/core/Launcher.cpp
    src/update/Version.cpp
    src/update/UpdateManifest.cpp
    src/update/SignatureVerifier.cpp
    src/update/WinHttpClient.cpp
    src/update/UpdatePackage.cpp
    src/update/RuntimeActivation.cpp
    src/update/UpdateManager.cpp
    src/update/RuntimeStateStore.cpp
    src/console/ConsoleRenderer.cpp
    src/console/Mascot.cpp
    src/console/Animation.cpp
    src/ui/Theme.cpp
    src/ui/FrameBuffer.cpp
    src/ui/Input.cpp
    src/ui/Rng.cpp
    src/ui/UiContext.cpp
    src/ui/Effects.cpp
    src/ui/Widgets.cpp
    src/ui/Watchdog.cpp
    src/app/UiScreens.cpp
    src/app/App.cpp
)

add_library(cheburnet_lib STATIC ${LIB_SOURCES})
target_include_directories(cheburnet_lib PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${GEN_DIR}")
target_compile_definitions(cheburnet_lib PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(cheburnet_lib PUBLIC bcrypt crypt32 winhttp advapi32 shell32 ole32 user32)

# Warnings / hardening for our own code.
set(CHEBURNET_WARN /W4 /WX /permissive- /utf-8)
set(CHEBURNET_HARDEN /guard:cf)
target_compile_options(cheburnet_lib PRIVATE ${CHEBURNET_WARN})
target_compile_options(cheburnet_lib PUBLIC ${CHEBURNET_HARDEN}
    $<$<CONFIG:Release>:/O2 /Zi>)

# ---------------------------------------------------------------------------
# Launcher executable -> CHEBURNET.exe
# ---------------------------------------------------------------------------
add_executable(CHEBURNET
    src/main.cpp
    resources/cheburnet.rc
    resources/generated/cheburnet_payload.rc)
set_target_properties(CHEBURNET PROPERTIES OUTPUT_NAME "CHEBURNET")
target_link_libraries(CHEBURNET PRIVATE cheburnet_lib)
target_compile_options(CHEBURNET PRIVATE ${CHEBURNET_WARN})

# RC compiler needs to find resource.h / app.manifest / app.ico next to the .rc.
target_include_directories(CHEBURNET PRIVATE
    "${CMAKE_SOURCE_DIR}/resources"
    "${GEN_DIR}")
set_source_files_properties(resources/cheburnet.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/resources;${GEN_DIR}")

# Only our manifest (embedded via the .rc); no linker-generated one.
# DEP (/NXCOMPAT) and ASLR (/DYNAMICBASE) are on by default; add CFG + PDB.
target_link_options(CHEBURNET PRIVATE
    /MANIFEST:NO
    /guard:cf
    /DYNAMICBASE
    /NXCOMPAT
    /HIGHENTROPYVA
    $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
