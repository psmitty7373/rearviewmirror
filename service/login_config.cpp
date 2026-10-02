#include "login_config.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

namespace rvm::login {

namespace {

// Owner Administrators; SYSTEM and Administrators get everything; the DACL is
// protected, so nothing from ProgramData's own (user-readable) rules leaks in.
constexpr wchar_t kMachineDirSddl[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

// The same for the settings key, plus read and set-value (KEY_READ |
// KEY_SET_VALUE, never WRITE_DAC) for the installing account, %s.
constexpr wchar_t kMachineSettingsSddl[] = L"O:BAD:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;0x2001b;;;%s)";

bool SystemOrAdmins(PSID sid) {
    return sid && (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
}

// The entry itself, never what it links to.
HANDLE OpenEntry(const std::wstring& path, DWORD access) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

// A real folder, not a link, owned by SYSTEM or Administrators, allowing
// nobody else anything.
bool Trusted(HANDLE dir) {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(dir, FileBasicInfo, &basic, sizeof(basic)) ||
        (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                        nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = SystemOrAdmins(owner) && dacl;   // A null DACL allows everyone everything.
    for (WORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        ok = GetAce(dacl, i, &ace);
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (ok && header->AceType != ACCESS_DENIED_ACE_TYPE) {
            ok = header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                 SystemOrAdmins(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
        }
    }
    LocalFree(sd);
    return ok;
}

// Out of the way: a link is removed (the link, not its target), anything else
// renamed, since its contents are not ours to delete.
bool RemoveUntrusted(const std::wstring& path, std::wstring& movedAside) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
    }
    movedAside = path + L".untrusted-" + std::to_wstring(GetTickCount64());
    return MoveFileExW(path.c_str(), movedAside.c_str(), 0);
}

}  // namespace

std::wstring InstallDir() {
    std::wstring dir;
    PWSTR programFiles = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        dir = std::wstring(programFiles) + L"\\RearViewMirror";
    }
    CoTaskMemFree(programFiles);
    return dir;
}

std::wstring MachineDir() {
    std::wstring dir;
    PWSTR data = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &data))) {
        dir = std::wstring(data) + L"\\RearViewMirror";
    }
    CoTaskMemFree(data);
    return dir;
}

bool SecureMachineDir(std::wstring& movedAside) {
    movedAside.clear();
    const std::wstring dir = MachineDir();
    if (dir.empty()) return false;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kMachineDirSddl, SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        return false;
    }
    constexpr DWORD kAccess = READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES;
    HANDLE handle = OpenEntry(dir, kAccess);
    const DWORD openError = GetLastError();
    bool ok = true;
    if (handle == INVALID_HANDLE_VALUE ? openError != ERROR_FILE_NOT_FOUND : !Trusted(handle)) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        ok = RemoveUntrusted(dir, movedAside);
    }
    if (ok && handle == INVALID_HANDLE_VALUE) {
        // Has to be new: one that appeared meanwhile is not ours either.
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        if (CreateDirectoryW(dir.c_str(), &sa)) handle = OpenEntry(dir, kAccess);
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    ok = ok && handle != INVALID_HANDLE_VALUE && GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
         GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
         SetSecurityInfo(handle, SE_FILE_OBJECT,
                         OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                             PROTECTED_DACL_SECURITY_INFORMATION,
                         owner, nullptr, dacl, nullptr) == ERROR_SUCCESS &&
         Trusted(handle);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    LocalFree(sd);
    return ok;
}

bool MachineDirTrusted() {
    const std::wstring dir = MachineDir();
    if (dir.empty()) return false;
    const HANDLE handle = OpenEntry(dir, READ_CONTROL | FILE_READ_ATTRIBUTES);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = Trusted(handle);
    CloseHandle(handle);
    return ok;
}

bool CreateMachineSettings() {
    // The account running this, elevated or not, is the one granted access.
    HANDLE token = nullptr;
    std::vector<uint8_t> user(SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER));
    DWORD size = 0;
    const bool haveUser = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                          GetTokenInformation(token, TokenUser, user.data(), static_cast<DWORD>(user.size()), &size);
    if (token) CloseHandle(token);
    wchar_t* sid = nullptr;
    if (!haveUser || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
        return false;
    }
    wchar_t sddl[256]{};
    swprintf_s(sddl, kMachineSettingsSddl, sid);
    LocalFree(sid);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    // Only administrators can create keys under HKLM\SOFTWARE, so one found
    // there was made by one; its rules are replaced all the same.
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    HKEY key = nullptr;
    bool ok = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kMachineSettingsKey, 0, nullptr, 0,
                              KEY_ALL_ACCESS | KEY_WOW64_64KEY, &sa, &key, nullptr) == ERROR_SUCCESS &&
              RegSetKeySecurity(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                         PROTECTED_DACL_SECURITY_INFORMATION, sd) == ERROR_SUCCESS;
    if (key) RegCloseKey(key);
    LocalFree(sd);
    return ok;
}

void DeleteMachineSettings() {
    HKEY software = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &software) ==
        ERROR_SUCCESS) {
        RegDeleteTreeW(software, L"RearViewMirror");
        RegCloseKey(software);
    }
}

bool LoadLoginSettings(StreamSettings& s) {
    if (!LoadMachineStreamSettings(s) || s.key.size() < kMinMachineKeyChars) return false;
    s.enabled = true;
    return true;
}

bool AppOwnerSid(std::vector<uint8_t>& sid) {
    sid.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMachineSettingsKey, 0, READ_CONTROL | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD size = 0;
    std::vector<uint8_t> sd;
    if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER) {
        sd.resize(size);
        if (RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, sd.data(), &size) != ERROR_SUCCESS) sd.clear();
    }
    RegCloseKey(key);
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (sd.empty() || !GetSecurityDescriptorDacl(sd.data(), &present, &dacl, &defaulted) || !dacl) return false;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace) || static_cast<const ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        const PSID owner = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
        if (SystemOrAdmins(owner)) continue;
        const auto* bytes = static_cast<const uint8_t*>(owner);
        sid.assign(bytes, bytes + GetLengthSid(owner));
        return true;
    }
    return false;
}


}  // namespace rvm::login
