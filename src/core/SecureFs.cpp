#include "SecureFs.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cwctype>
#include <vector>

#include "IntegrityVerifier.h"
#include "../util/Win32Error.h"

namespace cheburnet::securefs {
namespace {

constexpr wchar_t kDirectoryAclSddl[] =
    L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
constexpr wchar_t kFileAclSddl[] =
    L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";

Result Error(unsigned long code, std::wstring detail) {
    return {false, code, std::move(detail)};
}

Result WinError(std::wstring detail) {
    const DWORD code = ::GetLastError();
    detail += L": ";
    detail += win32::FormatError(code);
    return Error(code, std::move(detail));
}

std::wstring FullPath(const std::wstring& input) {
    const DWORD need = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (need == 0) return {};
    std::wstring output(need, L'\0');
    const DWORD written = ::GetFullPathNameW(input.c_str(), need, output.data(), nullptr);
    if (written == 0 || written >= need) return {};
    output.resize(written);
    while (output.size() > 3 && (output.back() == L'\\' || output.back() == L'/')) output.pop_back();
    return output;
}

std::wstring FoldPath(std::wstring value) {
    for (wchar_t& c : value) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}

bool WriteExact(HANDLE file, const void* data, std::size_t size) {
    const auto* current = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const DWORD chunk = static_cast<DWORD>(size > (1u << 20) ? (1u << 20) : size);
        DWORD written = 0;
        if (!::WriteFile(file, current, chunk, &written, nullptr) || written != chunk) return false;
        current += written;
        size -= written;
    }
    return true;
}

std::optional<std::wstring> RandomTempName(const std::wstring& parent) {
    std::array<unsigned char, 16> random{};
    if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, random.data(),
                                          static_cast<ULONG>(random.size()),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return std::nullopt;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = parent + L"\\.cheburnet-tmp-";
    name.reserve(name.size() + random.size() * 2);
    for (unsigned char byte : random) {
        name.push_back(hex[byte >> 4u]);
        name.push_back(hex[byte & 0x0Fu]);
    }
    return name;
}

std::wstring ParentOf(const std::wstring& path) {
    const std::wstring full = FullPath(path);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return full.substr(0, slash);
}

Result CheckHandle(HANDLE handle, ObjectKind kind, bool singleLink) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) return WinError(L"GetFileInformationByHandle");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return Error(ERROR_REPARSE_TAG_INVALID, L"точка повторного разбора отклонена");
    const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory != (kind == ObjectKind::Directory))
        return Error(ERROR_DIRECTORY, L"тип объекта не совпадает");
    if (kind == ObjectKind::File && singleLink && info.nNumberOfLinks != 1)
        return Error(ERROR_TOO_MANY_LINKS, L"файл с жёсткой ссылкой отклонён");
    return {true, 0, {}};
}

bool SidEquals(PSID left, PSID right) {
    return left && right && ::IsValidSid(left) && ::IsValidSid(right) &&
           ::EqualSid(left, right) != FALSE;
}

bool AclHasExactAllowAce(PACL dacl, PSID sid, DWORD mask, BYTE expectedFlags) {
    if (!dacl || !sid) return false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* raw = nullptr;
        if (!::GetAce(dacl, index, &raw) || !raw) return false;
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        const PSID aceSid = const_cast<DWORD*>(&ace->SidStart);
        if (SidEquals(aceSid, sid)) {
            return ace->Mask == mask && header->AceFlags == expectedFlags;
        }
    }
    return false;
}

Result VerifyProtectedSecurity(HANDLE handle, const std::wstring& path, ObjectKind kind) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD query = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (query != ERROR_SUCCESS)
        return Error(query, L"не удалось проверить защищённых владельца и DACL: " + path);

    BYTE adminSid[SECURITY_MAX_SID_SIZE]{};
    BYTE systemSid[SECURITY_MAX_SID_SIZE]{};
    BYTE usersSid[SECURITY_MAX_SID_SIZE]{};
    DWORD adminSize = sizeof(adminSid);
    DWORD systemSize = sizeof(systemSid);
    DWORD usersSize = sizeof(usersSid);
    const bool sidsOk =
        ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSize) &&
        ::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &systemSize) &&
        ::CreateWellKnownSid(WinBuiltinUsersSid, nullptr, usersSid, &usersSize);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool controlOk =
        ::GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE;
    ACL_SIZE_INFORMATION aclInfo{};
    const bool aclOk = dacl &&
        ::GetAclInformation(dacl, &aclInfo, sizeof(aclInfo), AclSizeInformation) != FALSE;
    const BYTE expectedAceFlags = kind == ObjectKind::Directory
        ? static_cast<BYTE>(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
        : static_cast<BYTE>(0);
    const bool verified = sidsOk && SidEquals(owner, adminSid) && controlOk &&
        (control & SE_DACL_PROTECTED) != 0 && aclOk && aclInfo.AceCount == 3 &&
        AclHasExactAllowAce(dacl, systemSid, FILE_ALL_ACCESS, expectedAceFlags) &&
        AclHasExactAllowAce(dacl, adminSid, FILE_ALL_ACCESS, expectedAceFlags) &&
        AclHasExactAllowAce(dacl, usersSid, 0x1200a9u, expectedAceFlags);
    ::LocalFree(descriptor);
    return verified ? Result{true, 0, {}}
                    : Error(ERROR_INVALID_SECURITY_DESCR,
                            L"проверка защищённых владельца и DACL не пройдена: " + path);
}

Result RemoveTreeImpl(const std::wstring& path) {
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {true, 0, {}};
        return Error(error, L"не удалось проверить объект очистки");
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        const BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(path.c_str())
                                                          : ::DeleteFileW(path.c_str());
        return ok ? Result{true, 0, {}} : WinError(L"не удалось удалить ссылку точки повторного разбора");
    }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return ::DeleteFileW(path.c_str()) ? Result{true, 0, {}} : WinError(L"не удалось удалить файл");
    }

    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW((path + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            Result child = RemoveTreeImpl(path + L"\\" + name);
            if (!child.ok) { ::FindClose(find); return child; }
        } while (::FindNextFileW(find, &data));
        const DWORD error = ::GetLastError();
        ::FindClose(find);
        if (error != ERROR_NO_MORE_FILES) return Error(error, L"перечисление объектов очистки завершилось ошибкой");
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
        return WinError(L"не удалось перечислить объект очистки");
    }
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return ::RemoveDirectoryW(path.c_str()) ? Result{true, 0, {}} : WinError(L"не удалось удалить каталог");
}

} // namespace

Result ValidateObject(const std::wstring& path, ObjectKind kind, bool requireSingleLink) {
    const DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL;
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (kind == ObjectKind::Directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = ::CreateFileW(path.c_str(), access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return WinError(L"не удалось открыть объект без перехода по точке повторного разбора");
    Result result = CheckHandle(handle, kind, requireSingleLink);
    ::CloseHandle(handle);
    if (!result.ok) result.detail += L": " + path;
    return result;
}

Result ValidatePathComponents(const std::wstring& path) {
    const std::wstring full = FullPath(path);
    if (full.size() < 3 || full[1] != L':' || full[2] != L'\\')
        return Error(ERROR_BAD_PATHNAME, L"допустимы только абсолютные пути с буквой диска");
    std::size_t end = 3;
    for (;;) {
        end = full.find(L'\\', end);
        const std::wstring component = end == std::wstring::npos ? full : full.substr(0, end);
        Result checked = ValidateObject(component, ObjectKind::Directory, false);
        if (!checked.ok) return checked;
        if (end == std::wstring::npos) break;
        ++end;
    }
    return {true, 0, {}};
}

Result ValidateProtectedObject(const std::wstring& path, ObjectKind kind,
                               bool requireSingleLink) {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (kind == ObjectKind::Directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return WinError(L"не удалось открыть защищённый объект для проверки");
    Result checked = CheckHandle(handle, kind, requireSingleLink);
    Result result = checked.ok ? VerifyProtectedSecurity(handle, path, kind) : checked;
    ::CloseHandle(handle);
    if (!result.ok && result.detail.empty()) result.detail = path;
    return result;
}

Result ValidateProtectedHandle(void* nativeHandle, const std::wstring& path,
                               ObjectKind kind, bool requireSingleLink) {
    HANDLE handle = static_cast<HANDLE>(nativeHandle);
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return Error(ERROR_INVALID_HANDLE, L"недействительный дескриптор защищённого объекта: " + path);
    Result checked = CheckHandle(handle, kind, requireSingleLink);
    Result result = checked.ok ? VerifyProtectedSecurity(handle, path, kind) : checked;
    if (!result.ok && result.detail.empty()) result.detail = path;
    return result;
}

Result HardenObject(const std::wstring& path, ObjectKind kind) {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (kind == ObjectKind::Directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return WinError(L"не удалось открыть объект для защиты владельца и DACL");
    Result checked = CheckHandle(handle, kind, kind == ObjectKind::File);
    if (!checked.ok) {
        ::CloseHandle(handle);
        checked.detail += L": " + path;
        return checked;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kind == ObjectKind::Directory ? kDirectoryAclSddl : kFileAclSddl,
            SDDL_REVISION_1, &descriptor, nullptr)) {
        Result result = WinError(L"не удалось создать защищённую ACL");
        ::CloseHandle(handle);
        return result;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present) {
        ::LocalFree(descriptor);
        Result result = WinError(L"не удалось прочитать защищённую ACL");
        ::CloseHandle(handle);
        return result;
    }
    BYTE adminSid[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(adminSid);
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidSize)) {
        ::LocalFree(descriptor);
        Result result = WinError(L"не удалось создать SID группы Administrators");
        ::CloseHandle(handle);
        return result;
    }
    // Apply and verify security through the same no-follow handle. This closes
    // the name-swap window that exists with path-based SetNamedSecurityInfo.
    const DWORD error = ::SetSecurityInfo(
        handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        adminSid, nullptr, dacl, nullptr);
    ::LocalFree(descriptor);
    if (error != ERROR_SUCCESS) {
        ::CloseHandle(handle);
        return Error(error, L"не удалось применить защищённых владельца и DACL: " + path);
    }
    Result object = CheckHandle(handle, kind, kind == ObjectKind::File);
    Result verified = object.ok ? VerifyProtectedSecurity(handle, path, kind) : object;
    ::CloseHandle(handle);
    if (!verified.ok && verified.detail.empty()) verified.detail = path;
    return verified;
}

Result EnsureProtectedDirectory(const std::wstring& path) {
    const std::wstring full = FullPath(path);
    if (full.size() < 3 || full[1] != L':' || full[2] != L'\\')
        return Error(ERROR_BAD_PATHNAME, L"недопустимый путь защищённого каталога");
    std::size_t end = 3;
    for (;;) {
        end = full.find(L'\\', end);
        const std::wstring component = end == std::wstring::npos ? full : full.substr(0, end);
        const DWORD attr = ::GetFileAttributesW(component.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (!::CreateDirectoryW(component.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
                return WinError(L"не удалось создать компонент защищённого каталога");
        }
        Result checked = ValidateObject(component, ObjectKind::Directory, false);
        if (!checked.ok) return checked;
        if (end == std::wstring::npos) break;
        ++end;
    }
    // Ancestors such as C:\ProgramData are validated but never modified.
    return HardenObject(full, ObjectKind::Directory);
}

Result AtomicWrite(const std::wstring& target, const void* data, std::size_t size,
                   const std::string& expectedSha256) {
    std::optional<std::string> memorySha = IntegrityVerifier::Sha256Hex(data, size);
    if (!memorySha) return Error(ERROR_CRC, L"не удалось вычислить SHA-256 буфера памяти");
    if (!expectedSha256.empty() && !IntegrityVerifier::HexEquals(*memorySha, expectedSha256))
        return Error(ERROR_CRC, L"SHA-256 исходного буфера не совпадает");

    return AtomicWriteStream(target, size, *memorySha,
                             [&](const StreamSink& sink) { return sink(data, size); });
}

Result AtomicWriteStream(const std::wstring& target, std::uint64_t expectedSize,
                         const std::string& expectedSha256,
                         const StreamProducer& producer) {
    if (!producer || expectedSha256.size() != 64)
        return Error(ERROR_INVALID_PARAMETER, L"недействительный контракт атомарного потока");
    const std::wstring parent = ParentOf(target);
    Result parentCheck = ValidatePathComponents(parent);
    if (!parentCheck.ok) return parentCheck;
    Result parentSecurity = ValidateProtectedObject(parent, ObjectKind::Directory, false);
    if (!parentSecurity.ok) return parentSecurity;
    const DWORD existing = ::GetFileAttributesW(target.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES) {
        Result existingCheck = ValidateObject(target, ObjectKind::File, true);
        if (!existingCheck.ok) return existingCheck;
        Result existingSecurity = HardenObject(target, ObjectKind::File);
        if (!existingSecurity.ok) return existingSecurity;
    }

    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto name = RandomTempName(parent);
        if (!name) return Error(ERROR_GEN_FAILURE, L"CNG не удалось создать случайное значение");
        temporary = std::move(*name);
        file = ::CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                             nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
                                 FILE_FLAG_WRITE_THROUGH,
                             nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() != ERROR_FILE_EXISTS) return WinError(L"не удалось создать уникальный временный файл");
    }
    if (file == INVALID_HANDLE_VALUE) return Error(ERROR_FILE_EXISTS, L"исчерпан лимит совпадений имён временных файлов");

    Result handleCheck = CheckHandle(file, ObjectKind::File, true);
    std::uint64_t streamed = 0;
    const StreamSink sink = [&](const void* chunk, std::size_t size) {
        if ((size != 0 && chunk == nullptr) || streamed > expectedSize ||
            size > expectedSize - streamed || !WriteExact(file, chunk, size)) return false;
        streamed += size;
        return true;
    };
    bool ok = handleCheck.ok && producer(sink) && streamed == expectedSize &&
              ::FlushFileBuffers(file);
    DWORD failure = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(temporary.c_str());
        return Error(failure, L"запись, сброс буферов или проверка типа временного файла не пройдены");
    }
    Result hardened = HardenObject(temporary, ObjectKind::File);
    if (!hardened.ok) {
        ::DeleteFileW(temporary.c_str());
        return hardened;
    }
    const auto diskSize = IntegrityVerifier::FileSize(temporary);
    const auto diskSha = IntegrityVerifier::Sha256File(temporary);
    if (!diskSize || *diskSize != expectedSize || !diskSha ||
        !IntegrityVerifier::HexEquals(*diskSha, expectedSha256)) {
        ::DeleteFileW(temporary.c_str());
        return Error(ERROR_CRC, L"проверка временного файла после записи не пройдена");
    }
    if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Result failureResult = WinError(L"атомарная активация завершилась ошибкой");
        ::DeleteFileW(temporary.c_str());
        return failureResult;
    }
    Result targetCheck = ValidateObject(target, ObjectKind::File, true);
    if (!targetCheck.ok) return targetCheck;
    Result targetAcl = HardenObject(target, ObjectKind::File);
    if (!targetAcl.ok) return targetAcl;
    const auto finalSize = IntegrityVerifier::FileSize(target);
    const auto finalSha = IntegrityVerifier::Sha256File(target);
    if (!finalSize || *finalSize != expectedSize || !finalSha ||
        !IntegrityVerifier::HexEquals(*finalSha, expectedSha256))
        return Error(ERROR_CRC, L"проверка активированного файла не пройдена");
    return {true, 0, {}};
}

Result AtomicWrite(const std::wstring& target, const std::string& data,
                   const std::string& expectedSha256) {
    return AtomicWrite(target, data.data(), data.size(), expectedSha256);
}

bool IsStrictDescendant(const std::wstring& root, const std::wstring& candidate) {
    std::wstring foldedRoot = FoldPath(FullPath(root));
    const std::wstring foldedCandidate = FoldPath(FullPath(candidate));
    if (foldedRoot.empty() || foldedCandidate.empty() || foldedCandidate.size() <= foldedRoot.size())
        return false;
    if (foldedRoot.back() != L'\\') foldedRoot.push_back(L'\\');
    return foldedCandidate.rfind(foldedRoot, 0) == 0;
}

Result RemoveTreeUnder(const std::wstring& allowedRoot, const std::wstring& target) {
    if (!IsStrictDescendant(allowedRoot, target))
        return Error(ERROR_ACCESS_DENIED, L"объект очистки не является строгим дочерним путём");
    Result rootCheck = ValidatePathComponents(allowedRoot);
    if (!rootCheck.ok) return rootCheck;
    const std::wstring parent = ParentOf(target);
    if (parent.empty()) return Error(ERROR_BAD_PATHNAME, L"у объекта очистки нет родительского пути");
    Result parentCheck = ValidatePathComponents(parent);
    if (!parentCheck.ok) return parentCheck;
    return RemoveTreeImpl(FullPath(target));
}

Result BootstrapProtectedTree(const std::wstring& programDataRoot,
                              const std::wstring& logsDir,
                              const std::wstring& runtimeRoot,
                              const std::wstring& updatesDir,
                              const std::wstring& userDir) {
    for (const std::wstring* path : {&programDataRoot, &logsDir, &runtimeRoot, &updatesDir, &userDir}) {
        Result result = EnsureProtectedDirectory(*path);
        if (!result.ok) return result;
    }
    Result userLists = EnsureProtectedDirectory(userDir + L"\\lists");
    if (!userLists.ok) return userLists;
    return {true, 0, {}};
}

} // namespace cheburnet::securefs
