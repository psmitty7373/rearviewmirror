#include "login_config.h"
#include "net/crypto.h"
#include "persist.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>

namespace rvm::login {

namespace {

// Owner Administrators; SYSTEM and Administrators get everything; the DACL is
// protected, so nothing from ProgramData's own (user-readable) rules leaks in.
constexpr wchar_t kMachineDirSddl[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

// Separates these blobs from anything else encrypted to the machine.
constexpr char kEntropy[] = "RearViewMirror.LoginKey";

std::wstring SettingsPath() {
    return MachineDir() + L"\\login.ini";
}

bool ProtectForMachine(const std::wstring& secret, std::vector<uint8_t>& blob) {
    DATA_BLOB in{ static_cast<DWORD>(secret.size() * sizeof(wchar_t)),
                  reinterpret_cast<BYTE*>(const_cast<wchar_t*>(secret.data())) };
    DATA_BLOB entropy{ sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Rear View Mirror sign-in key", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    blob.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool UnprotectForMachine(const std::vector<uint8_t>& blob, std::wstring& secret) {
    if (blob.empty()) return false;
    DATA_BLOB in{ static_cast<DWORD>(blob.size()), const_cast<BYTE*>(blob.data()) };
    DATA_BLOB entropy{ sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    secret.assign(reinterpret_cast<const wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

int ReadInt(const wchar_t* key, int fallback, const std::wstring& path) {
    return static_cast<int>(GetPrivateProfileIntW(L"Login", key, fallback, path.c_str()));
}

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

bool SaveLoginSettings(const StreamSettings& s) {
    std::vector<uint8_t> blob;
    if (s.key.size() < kMinLoginKeyChars || !ProtectForMachine(s.key, blob)) return false;
    std::wstring text = L"[Login]\r\n";
    text += L"Port=" + std::to_wstring(s.port) + L"\r\n";
    text += L"BitrateKbps=" + std::to_wstring(s.bitrateKbps) + L"\r\n";
    text += L"Fps=" + std::to_wstring(s.fps) + L"\r\n";
    text += L"Preset=" + std::to_wstring(static_cast<int>(s.preset)) + L"\r\n";
    text += L"KeyBlob=" + net::ToHex(blob) + L"\r\n";
    return WriteTextAtomically(SettingsPath(), text);
}

bool LoadLoginSettings(StreamSettings& s) {
    const std::wstring path = SettingsPath();
    wchar_t hex[4096]{};
    GetPrivateProfileStringW(L"Login", L"KeyBlob", L"", hex, ARRAYSIZE(hex), path.c_str());
    std::wstring key;
    if (!UnprotectForMachine(net::FromHex(hex), key) || key.size() < kMinLoginKeyChars) return false;
    s = StreamSettings{};
    s.enabled     = true;
    s.key         = std::move(key);
    s.port        = static_cast<uint16_t>(ClampI(ReadInt(L"Port", s.port, path), 1, 65535));
    s.bitrateKbps = static_cast<UINT>(ClampI(ReadInt(L"BitrateKbps", 8000, path), 1000, 100000));
    s.fps         = static_cast<UINT>(ClampI(ReadInt(L"Fps", 30, path), static_cast<int>(kMinStreamFps),
                                             static_cast<int>(kMaxStreamFps)));
    s.preset      = static_cast<EncoderPreset>(ClampI(ReadInt(L"Preset", 0, path), 0, 2));
    return true;
}

}  // namespace rvm::login
