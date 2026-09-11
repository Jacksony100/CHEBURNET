#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "UpdateManager.h"

namespace cheburnet {
class RuntimePaths;
}

namespace cheburnet::update {

enum class CheckPhase { Idle, Running, Ready, Taken, Cancelled };

// Выполняет одну проверку обновлений вне потока интерфейса, чтобы недоступный
// DNS, прокси или сеть не создавали впечатление зависшей программы.
//
// Правила владения и времени жизни (см. docs/UPDATE_SECURITY.md):
//   * рабочий поток никогда не обращается к интерфейсу и не вызывает владельца
//     обратно — он только кладёт CheckResult в состояние под мьютексом;
//   * Cancel() идемпотентен, выставляет отмену и присоединяет поток;
//     деструктор вызывает Cancel(), поэтому поток не может пережить объект и
//     не может увидеть разрушенное состояние;
//   * мьютекс никогда не удерживается во время сетевого ввода-вывода;
//   * отмена проходит в WinHttpClient, поэтому незавершённый запрос
//     прерывается, а не ожидается до тайм-аута.
//
// Модель доверия не меняется: рабочий поток вызывает тот же
// UpdateManager::CheckNow, то есть проверка подписи манифеста и оценка версий
// полностью совпадают с синхронным путём.
class UpdateCheckService {
public:
    // Тестовый шов. Вызываемый объект обязан соблюдать `cancel` и не должен
    // обращаться к интерфейсу.
    using CheckFunction = std::function<CheckResult(std::atomic<bool>& cancel)>;

    // `paths` обязан пережить сервис.
    explicit UpdateCheckService(const RuntimePaths& paths);
    explicit UpdateCheckService(CheckFunction check);
    ~UpdateCheckService();

    UpdateCheckService(const UpdateCheckService&) = delete;
    UpdateCheckService& operator=(const UpdateCheckService&) = delete;

    // Запускает фоновую проверку. Возвращает false, если проверка уже идёт,
    // результат ещё не забран, либо сервис был отменён.
    bool Start();

    // Неблокирующее получение результата. Возвращает true ровно один раз на
    // одну завершённую проверку. Вызывать необязательно: если контекст
    // интерфейса сменился, результат можно просто не забирать.
    bool TryTakeResult(CheckResult& result);

    CheckPhase Phase() const;

    // Идемпотентно: запрашивает отмену и присоединяет рабочий поток.
    void Cancel();

private:
    void Join();

    CheckFunction              check_;
    std::atomic<bool>          cancel_{false};
    mutable std::mutex         mutex_;
    std::optional<CheckResult> result_;
    CheckPhase                 phase_ = CheckPhase::Idle;
    std::thread                worker_;
};

} // namespace cheburnet::update
