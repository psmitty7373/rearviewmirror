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

bool SecureMachineDir() {
    const std::wstring dir = MachineDir();
    if (dir.empty()) return false;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kMachineDirSddl, SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    bool ok = CreateDirectoryW(dir.c_str(), &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (ok) {
        // Whether just made or found: the rules are applied either way, owner
        // included, so a folder planted in advance is not trusted as found.
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        PSID owner = nullptr;
        ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
             GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
             SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                       PROTECTED_DACL_SECURITY_INFORMATION,
                                   owner, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    }
    LocalFree(sd);
    return ok;
}

bool SaveLoginSettings(const StreamSettings& s) {
    std::vector<uint8_t> blob;
    if (s.key.empty() || !ProtectForMachine(s.key, blob)) return false;
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
    if (!UnprotectForMachine(net::FromHex(hex), key) || key.empty()) return false;
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
