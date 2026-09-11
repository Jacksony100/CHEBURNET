# ---------------------------------------------------------------------------
# CHEBURNET — единственный авторитетный источник версии.
#
# Здесь задаются только четыре поля. Всё остальное (семантическая версия,
# числовая PE-версия, признак предварительного выпуска и канал обновлений)
# выводится ниже, чтобы версия нигде не дублировалась вручную.
#
# Правила:
#   * CHEBURNET_VERSION_PRERELEASE == ""      -> стабильный выпуск vX.Y.Z
#   * CHEBURNET_VERSION_PRERELEASE == "rc.N"  -> предварительный выпуск vX.Y.Z-rc.N
#
# Числовая PE-версия намеренно отличается от семантической: PE-формат не умеет
# выражать prerelease, поэтому номер RC переносится в четвёртый компонент
# (1.0.0-rc.3 -> 1.0.0.3, стабильная 1.0.0 -> 1.0.0.0). PE-версия —
# информационная; решения об обновлении принимаются ИСКЛЮЧИТЕЛЬНО по
# семантической версии (CHEBURNET_VERSION_STR), см. docs/UPDATE_SECURITY.md.
# ---------------------------------------------------------------------------

set(CHEBURNET_VERSION_MAJOR 1)
set(CHEBURNET_VERSION_MINOR 0)
set(CHEBURNET_VERSION_PATCH 0)
set(CHEBURNET_VERSION_PRERELEASE "rc.4")

# --- производные значения: не редактировать ---------------------------------

foreach(_component MAJOR MINOR PATCH)
    if(NOT "${CHEBURNET_VERSION_${_component}}" MATCHES "^(0|[1-9][0-9]*)$")
        message(FATAL_ERROR
            "CHEBURNET_VERSION_${_component} должен быть числом без ведущих нулей")
    endif()
endforeach()

set(CHEBURNET_VERSION_CORE
    "${CHEBURNET_VERSION_MAJOR}.${CHEBURNET_VERSION_MINOR}.${CHEBURNET_VERSION_PATCH}")

if(CHEBURNET_VERSION_PRERELEASE STREQUAL "")
    set(CHEBURNET_VERSION_SEMANTIC "${CHEBURNET_VERSION_CORE}")
    set(CHEBURNET_VERSION_TWEAK 0)
    set(CHEBURNET_VERSION_IS_PRERELEASE 0)
    set(CHEBURNET_STABLE_CHANNEL 1)
    set(CHEBURNET_RELEASE_CHANNEL "stable")
elseif(CHEBURNET_VERSION_PRERELEASE MATCHES "^rc\.([1-9][0-9]*)$")
    set(CHEBURNET_VERSION_SEMANTIC
        "${CHEBURNET_VERSION_CORE}-${CHEBURNET_VERSION_PRERELEASE}")
    set(CHEBURNET_VERSION_TWEAK "${CMAKE_MATCH_1}")
    set(CHEBURNET_VERSION_IS_PRERELEASE 1)
    set(CHEBURNET_STABLE_CHANNEL 0)
    set(CHEBURNET_RELEASE_CHANNEL "prerelease")
else()
    message(FATAL_ERROR
        "CHEBURNET_VERSION_PRERELEASE должен быть пустым или иметь вид rc.N (N >= 1), "
        "получено: '${CHEBURNET_VERSION_PRERELEASE}'")
endif()

if(CHEBURNET_VERSION_TWEAK GREATER 65534)
    message(FATAL_ERROR "номер RC не укладывается в компонент PE-версии")
endif()

set(CHEBURNET_VERSION_PE "${CHEBURNET_VERSION_CORE}.${CHEBURNET_VERSION_TWEAK}")
