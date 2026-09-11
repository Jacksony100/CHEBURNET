#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../config/Config.h"

namespace cheburnet {
class RuntimePaths;
}

namespace cheburnet::diag {

// Экспорт диагностики CHEBURNET.
//
// Свойства, которые обязаны сохраняться (см. docs/PRIVACY.md):
//   * только локально: архив пишется по пути, который пользователь видит до
//     подтверждения; никакой отправки нет и быть не может;
//   * только по явному действию пользователя;
//   * ограниченный объём: хвост журнала обрезан, внешние пользовательские
//     списки не включаются;
//   * редактирование: имя пользователя, имя машины, пути профиля, адреса IP,
//     значения переменных окружения и похожие на токены строки заменяются
//     метками до записи;
//   * состав описан в manifest.json внутри самого архива.

// Одна запись архива: имя внутри архива и содержимое в UTF-8.
struct BundleEntry {
    std::string name;
    std::string content;
};

// Значения, которые подставляются вместо чувствительных данных. Вынесены
// отдельно, чтобы редактирование можно было проверить детерминированно.
struct RedactionContext {
    std::wstring userName;      // имя текущего пользователя
    std::wstring machineName;   // имя компьютера
    std::wstring userProfile;   // %USERPROFILE%
};

RedactionContext CurrentRedactionContext();

// Чистая функция: возвращает текст, пригодный для передачи третьей стороне.
// Заменяет имя пользователя, имя машины, пути профиля и каталога Users,
// адреса IPv4/IPv6 и длинные строки, похожие на токены.
std::wstring Redact(std::wstring_view text, const RedactionContext& context);

// Состав архива. Не обращается к сети и ничего не отправляет.
struct BundleOptions {
    std::size_t maxLogBytes = 256 * 1024;  // ограничение хвоста журнала
};

struct Bundle {
    std::vector<BundleEntry> entries;
    // Человекочитаемое описание состава для показа пользователю ДО записи.
    std::wstring preview;
};

Bundle BuildBundle(const RuntimePaths& paths, const Config& config,
                   const BundleOptions& options = {});

// Имя архива: CHEBURNET-diagnostics-YYYYMMDD-HHMMSS.zip (локальное время).
std::wstring SuggestedFileName();

// Минимальный ZIP без сжатия (метод store). Сторонняя библиотека не вводится
// ради одной операции; формат детерминирован и полностью проверяем.
bool WriteZipArchive(const std::wstring& path, const std::vector<BundleEntry>& entries,
                     std::wstring& error);

// Только для тестов: CRC-32 (полином IEEE), используемый записями ZIP.
std::uint32_t Crc32(const void* data, std::size_t size);

} // namespace cheburnet::diag
