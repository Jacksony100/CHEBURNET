add_executable(cheburnet_tests test_main.cpp)
target_link_libraries(cheburnet_tests PRIVATE cheburnet_lib)
target_compile_options(cheburnet_tests PRIVATE /W4 /WX /permissive- /utf-8)

set(CHEBURNET_TEST_PACKAGE "${CMAKE_BINARY_DIR}/tests/payload-test.cbpkg")
add_custom_command(
    OUTPUT "${CHEBURNET_TEST_PACKAGE}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/build-update-package.ps1"
            -OutFile "${CHEBURNET_TEST_PACKAGE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/build-update-package.ps1"
            "${CMAKE_SOURCE_DIR}/resources/generated/strategy-catalog.json"
            "${CMAKE_SOURCE_DIR}/resources/upstream/provenance.json"
    VERBATIM)
add_custom_target(cheburnet_test_package ALL DEPENDS "${CHEBURNET_TEST_PACKAGE}")
add_dependencies(cheburnet_tests cheburnet_test_package)
target_compile_definitions(cheburnet_tests PRIVATE
    CHEBURNET_TEST_PACKAGE=L"${CHEBURNET_TEST_PACKAGE}"
    CHEBURNET_VERSION_CASES=L"${CMAKE_CURRENT_SOURCE_DIR}/version-order-cases.json")

set(CHEBURNET_TEST_NAMES
    args quoting config sha256 manifest process integrity preflight securefs loghandle animation progress mascot
    layout checkpoints menu effects theme framebuffer configmigration)
list(APPEND CHEBURNET_TEST_NAMES
    jsonstrict updatemanifest updateversion updatesignature updatepackage updaterollback updatestate
    updatehttp updatecheckservice operationstate fuzzparsers faultinjection diagnostics runtimerecovery)

foreach(t ${CHEBURNET_TEST_NAMES})
    add_test(NAME ${t} COMMAND cheburnet_tests ${t})
endforeach()

add_test(NAME package_safety
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/package_safety.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")
add_test(NAME security_regression
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/security_regression.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")
add_test(NAME license_validation
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/validate-licenses.ps1"
            -Root "${CMAKE_SOURCE_DIR}")

add_test(NAME release_metadata
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/release_metadata.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}"
            -Launcher "$<TARGET_FILE:CHEBURNET>"
            -Payload "${CHEBURNET_TEST_PACKAGE}")

# Стабильный выпуск не может быть опубликован без действительной подписи.
add_test(NAME release_signing
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/release_signing.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")

# Оснастка релизного шлюза E2E проверяется сама: состав сценариев, защита
# окружения и схема доказательств. Привилегированные сценарии выполняются
# только на одноразовой машине, см. docs/production/E2E_RELEASE_GATE.md.
add_test(NAME e2e_harness
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/e2e/Invoke-CheburnetE2E.ps1" -SelfTest)

# Новый стабильный выпуск upstream обязан превращаться в устойчивую задачу,
# без автоматического слияния и без лишних прав.
add_test(NAME upstream_automation
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/upstream_automation.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")

# Изолированное дерево проверки импортёра обязано удовлетворять тем же
# релизным шлюзам, что и сам репозиторий.
add_test(NAME importer_validation_tree
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/importer_validation_tree.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")

# Скриптовая реализация модели/сравнения версий обязана совпадать с C++ на
# одной канонической таблице (tests/version-order-cases.json).
add_test(NAME version_model
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/version_model.ps1"
            -RepositoryRoot "${CMAKE_SOURCE_DIR}")
