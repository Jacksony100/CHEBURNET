#include "Version.h"

#include <charconv>

namespace cheburnet::update {
namespace {

constexpr std::size_t kMaxCoreParts = 8;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsIdentifierChar(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// Разбирает набор идентификаторов, разделённых точками. Пустой идентификатор
// отклоняется: "rc." и "rc..1" не должны становиться псевдонимами "rc".
bool SplitIdentifiers(std::string_view value, std::vector<std::string>& output) {
    if (value.empty()) return false;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t dot = value.find('.', pos);
        const std::string_view item =
            value.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
        if (item.empty() || item.size() > 48) return false;
        for (const char c : item) {
            if (!IsIdentifierChar(c)) return false;
        }
        output.emplace_back(item);
        if (dot == std::string_view::npos) return true;
        pos = dot + 1;
        if (pos == value.size()) return false;  // завершающая точка
    }
}

bool IsNumericIdentifier(const std::string& value) {
    for (const char c : value) {
        if (!IsDigit(c)) return false;
    }
    // Ведущие нули запрещены: "01" не должно быть псевдонимом "1".
    return !value.empty() && (value.size() == 1 || value.front() != '0');
}

// Precedence идентификаторов prerelease по SemVer 2.0.0.
int CompareIdentifiers(const std::vector<std::string>& left,
                       const std::vector<std::string>& right) {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < count; ++i) {
        const bool leftNumeric = IsNumericIdentifier(left[i]);
        const bool rightNumeric = IsNumericIdentifier(right[i]);
        if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
        if (leftNumeric) {
            unsigned long long l = 0, r = 0;
            const auto lp = std::from_chars(left[i].data(), left[i].data() + left[i].size(), l);
            const auto rp = std::from_chars(right[i].data(), right[i].data() + right[i].size(), r);
            // Оба идентификатора состоят только из цифр, поэтому отказ возможен
            // лишь при переполнении. Тогда порядок остаётся строгим и
            // детерминированным за счёт лексикографического сравнения.
            if (lp.ec != std::errc{} || rp.ec != std::errc{}) {
                if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
                continue;
            }
            if (l != r) return l < r ? -1 : 1;
            continue;
        }
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

} // namespace

std::optional<Version> ParseVersion(std::string_view value) {
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.remove_prefix(1);
    if (value.empty() || value.size() > 64) return std::nullopt;
    Version result;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t start = pos;
        while (pos < value.size() && IsDigit(value[pos])) ++pos;
        if (start == pos) return std::nullopt;
        // Ведущие нули в core запрещены: "01.0.0" не должно быть псевдонимом
        // "1.0.0", иначе каноническое сравнение для защиты от понижения версии
        // перестаёт быть однозначным.
        if (pos - start > 1 && value[start] == '0') return std::nullopt;
        unsigned long long part = 0;
        const auto parsed = std::from_chars(value.data() + start, value.data() + pos, part);
        if (parsed.ec != std::errc{}) return std::nullopt;
        result.parts.push_back(part);
        if (result.parts.size() > kMaxCoreParts) return std::nullopt;
        if (pos == value.size()) break;
        if (value[pos] == '.') {
            ++pos;
            // За разделителем обязано следовать число. Приняв "1.", мы создали
            // бы псевдоним "1" и ослабили каноническое сравнение.
            if (pos == value.size()) return std::nullopt;
            continue;
        }
        break;
    }
    if (result.parts.empty()) return std::nullopt;
    if (pos >= value.size()) return result;

    result.suffix.assign(value.substr(pos));
    std::string_view rest = value.substr(pos);

    // Ревизия upstream: ровно одна строчная латинская буква сразу после core.
    if (rest.front() >= 'a' && rest.front() <= 'z') {
        result.revision.assign(rest.substr(0, 1));
        rest.remove_prefix(1);
    }

    // Build-метаданные в сравнении не участвуют, но обязаны быть корректными.
    const std::size_t plus = rest.find('+');
    if (plus != std::string_view::npos) {
        std::vector<std::string> build;
        if (!SplitIdentifiers(rest.substr(plus + 1), build)) return std::nullopt;
        rest = rest.substr(0, plus);
    }

    if (!rest.empty()) {
        if (rest.front() != '-') return std::nullopt;
        if (!SplitIdentifiers(rest.substr(1), result.prereleaseIds)) return std::nullopt;
        result.prerelease = true;
    }
    return result;
}

int CompareVersions(const Version& left, const Version& right) {
    const std::size_t count = left.parts.size() > right.parts.size() ? left.parts.size()
                                                                    : right.parts.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto l = i < left.parts.size() ? left.parts[i] : 0;
        const auto r = i < right.parts.size() ? right.parts[i] : 0;
        if (l < r) return -1;
        if (l > r) return 1;
    }
    if (left.revision != right.revision) {
        if (left.revision.empty()) return -1;  // 1.10.1 < 1.10.1a
        if (right.revision.empty()) return 1;
        return left.revision < right.revision ? -1 : 1;
    }
    if (left.prerelease != right.prerelease) {
        return left.prerelease ? -1 : 1;       // 1.0.0-rc.3 < 1.0.0
    }
    if (!left.prerelease) return 0;
    return CompareIdentifiers(left.prereleaseIds, right.prereleaseIds);
}

} // namespace cheburnet::update
