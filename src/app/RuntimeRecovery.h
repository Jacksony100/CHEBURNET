#pragma once

#include <string>
#include <string_view>

namespace cheburnet {

// Решения восстановления при запуске.
//
// Раньше эта логика жила внутри конструктора App вместе с работой с реальными
// процессами, файловой системой и ProgramData, поэтому её нельзя было проверить
// иначе как запуском всей программы с правами администратора. Здесь она
// выражена чистыми функциями от наблюдаемого состояния: поведение не меняется,
// но каждая ветка становится проверяемой напрямую.
//
// Ни одна из функций ничего не выполняет: они только сообщают, что должно
// произойти. Выполнение остаётся за App, где есть доступ к процессам и диску.

// Что делать с состоянием pending, оставшимся от прерванного обновления.
enum class PendingAction {
    // Ожидающего состояния нет.
    None,
    // Работает именно ожидающая среда: её нужно остановить, затем очистить
    // ожидание. Ожидающая версия НИКОГДА не повышается до текущей.
    StopPendingRuntimeThenClear,
    // Ничего не работает либо работает текущая среда: достаточно очистить
    // ожидание.
    ClearPendingOnly,
    // Работает среда, которая не является ни текущей, ни ожидающей. Приводить
    // состояние в порядок молча нельзя: личность процесса и active-runtime.json
    // разошлись бы. Запуск отклоняется.
    RefuseUnknownRuntime,
};

// `runningImagePath` имеет смысл только при `processValid`.
PendingAction DecidePendingRecovery(bool hasPending, bool processValid,
                                    std::wstring_view runningImagePath,
                                    std::wstring_view pendingWinwsPath,
                                    std::wstring_view currentWinwsPath);

// Что делать с активной рабочей средой при запуске.
enum class IntegrityAction {
    // Активная версия совпадает со встроенной: используется встроенный каталог.
    UseEmbedded,
    // Активная установленная версия прошла проверку целостности.
    UseInstalled,
    // Активная версия не прошла проверку, но есть предыдущая рабочая версия.
    RollbackToPrevious,
    // Активная версия не прошла проверку и откатываться некуда.
    Refuse,
};

IntegrityAction DecideIntegrityRecovery(bool runtimeIsEmbeddedVersion, bool activeVerified,
                                        bool haveTrustedState, bool havePreviousKnownGood);

} // namespace cheburnet
