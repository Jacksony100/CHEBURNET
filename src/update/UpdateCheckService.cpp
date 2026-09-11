#include "UpdateCheckService.h"

#include <utility>

#include "../core/RuntimePaths.h"

namespace cheburnet::update {

UpdateCheckService::UpdateCheckService(CheckFunction check) : check_(std::move(check)) {}

UpdateCheckService::UpdateCheckService(const RuntimePaths& paths)
    : UpdateCheckService([&paths](std::atomic<bool>& cancel) {
          // Ссылка на paths безопасна: владелец обязан пережить сервис, а
          // сервис присоединяет рабочий поток до собственного разрушения.
          const UpdateManager manager(paths);
          return manager.CheckNow(true, &cancel);
      }) {}

UpdateCheckService::~UpdateCheckService() { Cancel(); }

bool UpdateCheckService::Start() {
    {
        std::scoped_lock lock(mutex_);
        if (phase_ == CheckPhase::Running || phase_ == CheckPhase::Ready ||
            phase_ == CheckPhase::Cancelled) {
            return false;
        }
        if (!check_) return false;
        phase_ = CheckPhase::Running;
    }
    // Предыдущий поток (после Taken) присоединяется до запуска нового, чтобы
    // std::thread никогда не перезаписывался присоединяемым объектом.
    Join();
    cancel_.store(false, std::memory_order_release);
    worker_ = std::thread([this] {
        CheckResult produced;
        try {
            produced = check_(cancel_);
        } catch (...) {
            // Фоновая проверка обновлений никогда не должна валить программу:
            // отказ проверки не мешает работать с уже установленной средой.
            produced = CheckResult{};
            produced.status = CheckStatus::Offline;
            produced.message = L"Проверка обновлений завершилась ошибкой.";
        }
        std::scoped_lock lock(mutex_);
        result_ = std::move(produced);
        if (phase_ == CheckPhase::Running) phase_ = CheckPhase::Ready;
    });
    return true;
}

bool UpdateCheckService::TryTakeResult(CheckResult& result) {
    std::scoped_lock lock(mutex_);
    if (phase_ != CheckPhase::Ready || !result_) return false;
    result = std::move(*result_);
    result_.reset();
    phase_ = CheckPhase::Taken;
    return true;
}

CheckPhase UpdateCheckService::Phase() const {
    std::scoped_lock lock(mutex_);
    return phase_;
}

void UpdateCheckService::Cancel() {
    cancel_.store(true, std::memory_order_release);
    Join();
    std::scoped_lock lock(mutex_);
    phase_ = CheckPhase::Cancelled;
    result_.reset();
}

void UpdateCheckService::Join() {
    // Присоединение выполняется без удержания мьютекса: рабочий поток берёт
    // его в самом конце, поэтому join под мьютексом означал бы взаимоблокировку.
    if (worker_.joinable()) worker_.join();
}

} // namespace cheburnet::update
