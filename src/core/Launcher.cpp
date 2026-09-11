#include "Launcher.h"

#include <windows.h>

#include <string>
#include <vector>

#include "../util/CommandLine.h"
#include "../util/Logger.h"
#include "../util/StringUtil.h"
#include "../util/Win32Error.h"
#include "GeneratedManifest.h"
#include "IntegrityVerifier.h"
#include "PrivilegeManager.h"
#include "ResourceExtractor.h"
#include "SecureFs.h"
#include "StatusProbe.h"

namespace cheburnet {
namespace {

const std::vector<ConnectCheckpoint> kCheckpoints = {
    {1, 1, 5, L"Определение версии Windows"},
    {2, 1, 8, L"Проверка архитектуры x64"},
    {3, 1, 12, L"Проверка прав администратора"},
    {4, 1, 18, L"Подготовка защищённой среды"},
    {5, 2, 24, L"Проверка манифеста ресурсов"},
    {6, 2, 34, L"Извлечение компонентов"},
    {7, 2, 44, L"Проверка SHA-256"},
    {8, 2, 50, L"Применение owner/DACL policy"},
    {9, 3, 55, L"Загрузка config.json"},
    {10, 3, 60, L"Подготовка выбранной стратегии"},
    {11, 3, 64, L"Проверка списков адресов"},
    {12, 3, 68, L"Формирование командной строки"},
    {13, 4, 72, L"Предзапусковая проверка целостности"},
    {14, 4, 76, L"Создание объекта задания"},
    {15, 4, 82, L"Запуск winws.exe"},
    {16, 4, 88, L"Проверка идентичности процесса"},
    {17, 4, 92, L"Проверка стабильности процесса"},
    {18, 5, 95, L"Регистрация активной сессии"},
    {19, 5, 98, L"Запуск наблюдения за процессом"},
    {20, 5, 100, L"Завершение инициализации интерфейса"},
};

// Read up to `maxLines` trailing lines of a (small) UTF-8 log file.
std::wstring ReadTail(const std::wstring& path, int maxLines) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) {
        ::CloseHandle(h);
        return {};
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        ::CloseHandle(h);
        return {};
    }
    const long long kMax = 64 * 1024;
    long long toRead = size.QuadPart > kMax ? kMax : size.QuadPart;
    if (toRead > 0) {
        LARGE_INTEGER off;
        off.QuadPart = size.QuadPart - toRead;
        if (!::SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
            ::CloseHandle(h);
            return {};
        }
    }
    std::string content;
    content.resize(static_cast<size_t>(toRead));
    DWORD read = 0;
    if (toRead > 0 &&
        (!::ReadFile(h, content.data(), static_cast<DWORD>(toRead), &read, nullptr) ||
         read != static_cast<DWORD>(toRead))) {
        ::CloseHandle(h);
        return {};
    }
    content.resize(read);
    ::CloseHandle(h);

    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line =
            content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
        auto t = str::Trim(line);
        if (!t.empty()) lines.emplace_back(t);
    }
    std::wstring out;
    int start = static_cast<int>(lines.size()) - maxLines;
    if (start < 0) start = 0;
    for (int i = start; i < static_cast<int>(lines.size()); ++i) {
        if (!out.empty()) out += L"\n";
        out += str::ToUtf16(lines[static_cast<size_t>(i)]);
    }
    return out;
}

} // namespace

const std::vector<ConnectCheckpoint>& ConnectCheckpoints() { return kCheckpoints; }

std::wstring Launcher::StdoutLogPath() const {
    return paths_.LogsDir() + L"\\winws-stdout.log";
}
std::wstring Launcher::StderrLogPath() const {
    return paths_.LogsDir() + L"\\winws-stderr.log";
}

ConnectResult Launcher::Connect(const RuntimeStrategy& strategy, GameFilterMode gameFilter,
                                ui::UiEventQueue& events) {
    ConnectResult r;
    Logger::Info(L"подключение: стратегия=" + str::ToUtf16(strategy.id) + L" игровой_фильтр=" +
                 str::ToUtf16(GameFilterModeName(gameFilter)));

    const auto& CP = kCheckpoints;
    int floor = 0;
    auto cp = [&](int i) -> const ConnectCheckpoint& { return CP[static_cast<size_t>(i - 1)]; };
    auto started = [&](int i) {
        ui::UiEvent e;
        e.type = ui::UiEventType::StageStarted;
        e.checkpoint = i;
        e.percent = floor;
        e.severity = ui::Severity::Info;
        e.label = cp(i).label;
        events.Push(std::move(e));
    };
    auto okcp = [&](int i, ui::Severity sev = ui::Severity::Ok, std::wstring detail = {}) {
        floor = cp(i).percent;
        ui::UiEvent e;
        e.type = ui::UiEventType::StageOk;
        e.checkpoint = i;
        e.percent = floor;
        e.severity = sev;
        e.label = cp(i).label;
        e.detail = std::move(detail);
        events.Push(std::move(e));
    };
    auto failcp = [&](int i, std::wstring detail) {
        ui::UiEvent e;
        e.type = ui::UiEventType::StageFailed;
        e.checkpoint = i;
        e.percent = floor;
        e.severity = ui::Severity::Error;
        e.label = cp(i).label;
        e.detail = std::move(detail);
        events.Push(std::move(e));
    };
    auto fail = [&](int i, std::wstring msg, std::wstring detail) {
        failcp(i, detail);
        r.kind = ConnectKind::Error;
        r.message = std::move(msg);
        r.detail = std::move(detail);
            Logger::Error(L"подключение прервано на контрольной точке " + std::to_wstring(i));
        return r;
    };

    ResourceExtractor extractor(paths_);

    // ---- Phase 1: SYSTEM ----
    started(1);
    if (!probe::IsWindows10OrGreater())
        return fail(1, L"Требуется Windows 10 или 11.", L"Версия ОС ниже поддерживаемой.");
    okcp(1);
    started(2);
    if (!probe::Is64BitWindows())
        return fail(2, L"Требуется 64-битная Windows.", L"Архитектура x64 не обнаружена.");
    okcp(2);
    started(3);
    if (!PrivilegeManager::IsElevated())
        return fail(3, L"Нет прав администратора.",
                    L"Запустите CHEBURNET от имени администратора.");
    okcp(3);
    started(4);
    if (!securefs::EnsureProtectedDirectory(paths_.RuntimeVersionDir()).ok ||
        !securefs::EnsureProtectedDirectory(paths_.BinDir()).ok ||
        !securefs::EnsureProtectedDirectory(paths_.ListsDir()).ok)
        return fail(4, L"Не удалось подготовить рабочую директорию.", paths_.RuntimeVersionDir());
    if (!extractor.HardenRuntimeDirs())
        return fail(4, L"Не удалось защитить рабочую директорию (ACL).",
                    L"Проверьте права администратора и владельца:\n" + paths_.ProgramDataRoot());
    okcp(4);

    // ---- Conflict / already-running checks ----
    if (pm_.IsConnectedByUs()) {
        r.kind = ConnectKind::AlreadyRunningOurs;
        r.message = L"CHEBURNET уже подключён (winws.exe запущен ранее).";
        ui::UiEvent e{ui::UiEventType::Info, 0, floor, ui::Severity::Warn, r.message, L""};
        events.Push(std::move(e));
        return r;
    }
    if (probe::ServiceActive(L"zapret")) {
        r.kind = ConnectKind::AlreadyRunningService;
        r.message = L"Активна служба zapret. Дублирующий процесс не запускается.";
        r.detail = L"Остановите службу zapret, чтобы использовать CHEBURNET напрямую.";
        events.Push({ui::UiEventType::Info, 0, floor, ui::Severity::Warn, r.message, r.detail});
        return r;
    }
    {
        auto winwsProcs = probe::FindProcesses(L"winws.exe");
        const ProcessRecord candidate = pm_.Record();
        const unsigned long ourPid = ProcessManager::TrustedPidForExclusion(
            candidate, paths_.RuntimeRoot());
        bool foreign = false;
        for (const auto& p : winwsProcs)
            if (p.pid != ourPid) foreign = true;
        if (foreign) {
            r.kind = ConnectKind::ForeignWinws;
            r.message = L"Обнаружен winws.exe, запущенный не через CHEBURNET.";
            r.detail = L"Остановите его вручную. CHEBURNET не завершает чужие процессы.";
            events.Push({ui::UiEventType::Info, 0, floor, ui::Severity::Warn, r.message, r.detail});
            return r;
        }
    }

    // ---- Phase 2: PAYLOAD ----
    // Пустой встроенный resource manifest — ошибка генерации, а не состояние
    // времени выполнения, поэтому проверка вынесена на этап компиляции: так
    // сборка падает сразу, а не у пользователя при запуске (MSVC /analyze C6326).
    static_assert(kEmbeddedResourceCount > 0,
                  "embedded resource manifest must not be empty");
    started(5);
    okcp(5);
    started(6);
    ExtractionResult ex = extractor.EnsureExtracted();
    if (!ex.ok) return fail(6, L"Ошибка подготовки компонентов.", ex.error);
    okcp(6);
    started(7);
    okcp(7); // SHA-256 verified inside EnsureExtracted
    started(8);
    okcp(8); // owner/DACL applied per file inside EnsureExtracted
    Logger::Info(L"ресурсы готовы (извлечено=" + std::to_wstring(ex.extracted) + L", найдено=" +
                 std::to_wstring(ex.present) + L")");

    // ---- Phase 3: CONFIGURATION ----
    started(9);
    okcp(9);
    started(10);
    okcp(10);
    started(11);
    {
        const wchar_t* lists[] = {L"list-general.txt", L"list-exclude.txt", L"list-google.txt",
                                  L"ipset-all.txt", L"ipset-exclude.txt"};
        bool listsOk = true;
        for (const wchar_t* l : lists) {
            if (!IntegrityVerifier::FileSize(paths_.ListsDir() + L"\\" + l)) listsOk = false;
        }
        if (listsOk)
            okcp(11);
        else
            okcp(11, ui::Severity::Warn, L"некоторые списки отсутствуют");
    }
    started(12);
    std::vector<std::wstring> args =
        strategies::BuildArguments(strategy, paths_.BinDir(), paths_.ListsDir(), gameFilter,
                                   paths_.UserDir() + L"\\lists");
    const std::wstring exe = paths_.WinwsExePath();
    Logger::Info(L"аргументы winws готовы: файл=" + exe +
                 L" количество_аргументов=" + std::to_wstring(args.size()));
    okcp(12);

    // ---- Phase 4: ENGINE ----
    started(13);
    std::wstring firstBad;
    if (!extractor.VerifyBinaries(firstBad))
        return fail(13, L"Компонент изменён после проверки.", L"Не совпал: " + firstBad);
    okcp(13);
    started(14);
    okcp(14); // job object is created inside ProcessManager::Start
    started(15);
    StartResult sr = pm_.Start(exe, args, paths_.BinDir(), StdoutLogPath(), StderrLogPath());
    if (sr.status != StartStatus::Ok && sr.status != StartStatus::ExitedEarly)
        return fail(15, L"Не удалось запустить сетевой движок (winws.exe).",
                    L"Транзакция запуска отклонена. Windows " +
                        win32::FormatError(sr.win32Error));
    okcp(15);
    started(16);
    if (!sr.record.valid())
        return fail(16, L"Не удалось подтвердить идентичность процесса.", L"Нет записи процесса.");
    okcp(16);
    started(17);
    if (sr.status == StartStatus::ExitedEarly) {
        std::wstring tail = ReadTail(StderrLogPath(), 6);
        std::wstring detail = L"Код выхода winws.exe: " + std::to_wstring(sr.exitCode);
        if (!tail.empty()) detail += L"\nПоследние строки stderr:\n" + tail;
        return fail(17, L"Сетевой движок завершился сразу после запуска.", detail);
    }
    okcp(17);

    // ---- Phase 5: LINK ----
    started(18);
    okcp(18); // SaveRecord done inside Start
    started(19);
    okcp(19); // watchdog is armed by the connected screen
    started(20);
    okcp(20); // 100% - only reached after checkpoint 17 confirmed the process

    ui::UiEvent started_ev;
    started_ev.type = ui::UiEventType::ProcessStarted;
    started_ev.severity = ui::Severity::Ok;
    started_ev.percent = 100;
    started_ev.label = L"winws.exe pid " + std::to_wstring(sr.record.pid);
    events.Push(std::move(started_ev));

    r.kind = ConnectKind::Connected;
    r.message = L"CHEBURNET активен.";
    r.detail = L"Стратегия: " + strategy.displayName + L"  |  PID: " +
               std::to_wstring(sr.record.pid);
    Logger::Info(L"подключение успешно, pid=" + std::to_wstring(sr.record.pid));
    return r;
}

} // namespace cheburnet
