#include "stream_settings.h"
#include "net/codec.h"
#include "net/crypto.h"
#include "persist.h"
#include "resource.h"

#if RVM_LOGIN_SERVICE
#include <wincrypt.h>
#endif

namespace rvm {

namespace {

std::wstring SettingsPath() {
    return ConfigDir() + L"\\stream.ini";
}

#if RVM_LOGIN_SERVICE
// Separates these blobs from anything else encrypted to the machine.
constexpr char kMachineEntropy[] = "RearViewMirror.LoginKey";

bool ProtectForMachine(const std::wstring& secret, std::vector<uint8_t>& blob) {
    DATA_BLOB in{ static_cast<DWORD>(secret.size() * sizeof(wchar_t)),
                  reinterpret_cast<BYTE*>(const_cast<wchar_t*>(secret.data())) };
    DATA_BLOB entropy{ sizeof(kMachineEntropy) - 1,
                       reinterpret_cast<BYTE*>(const_cast<char*>(kMachineEntropy)) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Rear View Mirror stream key", &entropy, nullptr, nullptr,
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
    DATA_BLOB entropy{ sizeof(kMachineEntropy) - 1,
                       reinterpret_cast<BYTE*>(const_cast<char*>(kMachineEntropy)) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    secret.assign(reinterpret_cast<const wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

HKEY OpenMachineSettings(REGSAM access) {
    HKEY key = nullptr;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, kMachineSettingsKey, 0, access | KEY_WOW64_64KEY, &key) ==
                   ERROR_SUCCESS
               ? key
               : nullptr;
}

int ReadDword(HKEY key, const wchar_t* name, int fallback) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
               ? static_cast<int>(value)
               : fallback;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
           ERROR_SUCCESS;
}
#endif

size_t MinKeyChars() {
#if RVM_LOGIN_SERVICE
    if (MachineSettingsInUse()) return kMinMachineKeyChars;
#endif
    return 8;
}

std::wstring ReadStr(const wchar_t* key, const std::wstring& path) {
    // DPAPI blobs run to a few hundred bytes; hex doubles that.
    wchar_t buffer[4096]{};
    GetPrivateProfileStringW(L"Stream", key, L"", buffer, ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

int ReadInt(const wchar_t* key, int fallback, const std::wstring& path) {
    return static_cast<int>(GetPrivateProfileIntW(L"Stream", key, fallback, path.c_str()));
}

std::wstring RandomKey() {
    // Unambiguous characters only; this gets typed on the other machine.
    static const wchar_t alphabet[] = L"abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    constexpr size_t alphabetLen = ARRAYSIZE(alphabet) - 1;
    uint8_t raw[20];
    net::RandomBytes(raw, sizeof(raw));
    std::wstring key;
    for (uint8_t b : raw) key += alphabet[b % alphabetLen];
    return key;
}

struct DialogState {
    StreamSettings*      settings;
    const StreamControl* control;
    bool                 encoderAvailable = false;
};

HWND g_openDialog = nullptr;
constexpr UINT_PTR kStatusTimer = 1;

std::wstring GetText(HWND dlg, int id) {
    wchar_t buffer[512]{};
    GetDlgItemTextW(dlg, id, buffer, ARRAYSIZE(buffer));
    return buffer;
}

// Reads and validates the fields. `needKey`: starting needs a usable key,
// merely saving does not.
bool ReadFields(HWND dlg, const StreamSettings& base, bool needKey, StreamSettings& out) {
    out = base;
    BOOL ok = FALSE;
    const UINT port = GetDlgItemInt(dlg, IDC_STREAM_PORT, &ok, FALSE);
    if (!ok || port == 0 || port > 65535) {
        MessageBoxW(dlg, L"Enter a UDP port between 1 and 65535.", kAppName, MB_OK | MB_ICONWARNING);
        return false;
    }
    const UINT mbps = GetDlgItemInt(dlg, IDC_STREAM_BITRATE, &ok, FALSE);
    if (!ok || mbps < 1 || mbps > 100) {
        MessageBoxW(dlg, L"Enter a bitrate between 1 and 100 Mbps.", kAppName, MB_OK | MB_ICONWARNING);
        return false;
    }
    const UINT fps = GetDlgItemInt(dlg, IDC_STREAM_FPS, &ok, FALSE);
    if (!ok || fps < kMinStreamFps || fps > kMaxStreamFps) {
        MessageBoxW(dlg, L"Enter a frame rate between 1 and 240 fps.", kAppName, MB_OK | MB_ICONWARNING);
        return false;
    }
    out.port = static_cast<uint16_t>(port);
    out.bitrateKbps = mbps * 1000;
    out.fps = fps;
    const LRESULT pick = SendDlgItemMessageW(dlg, IDC_STREAM_PRESET, CB_GETCURSEL, 0, 0);
    if (pick != CB_ERR) {
        out.preset = static_cast<EncoderPreset>(
            SendDlgItemMessageW(dlg, IDC_STREAM_PRESET, CB_GETITEMDATA, static_cast<WPARAM>(pick), 0));
    }
    out.key = GetSecretText(dlg, IDC_STREAM_KEY);
    const size_t minKey = MinKeyChars();
    if ((needKey || !out.key.empty()) && out.key.size() < minKey) {
        const std::wstring text =
            minKey > 8 ? L"The sign-in screen service shares this key, so it needs at least " +
                             std::to_wstring(minKey) + L" characters. Press Generate."
                       : L"The shared key needs at least 8 characters. Generate one, or type your own.";
        MessageBoxW(dlg, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void RefreshStatus(HWND dlg, const DialogState& state) {
    const bool running = state.control->running();
    SetDlgItemTextW(dlg, IDC_STREAM_STATUS, state.control->status().c_str());
    SetDlgItemTextW(dlg, IDC_STREAM_TOGGLE, running ? L"Stop" : L"Start");
    EnableWindow(GetDlgItem(dlg, IDC_STREAM_TOGGLE), running || state.encoderAvailable);
}

INT_PTR CALLBACK StreamDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<DialogState*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        g_openDialog = dlg;
        const StreamSettings& s = *state->settings;

        SetDlgItemInt(dlg, IDC_STREAM_PORT, s.port, FALSE);
        SetDlgItemTextW(dlg, IDC_STREAM_KEY, s.key.c_str());
        SetDlgItemInt(dlg, IDC_STREAM_BITRATE, (std::max)(s.bitrateKbps / 1000, 1u), FALSE);
        SetDlgItemInt(dlg, IDC_STREAM_FPS, s.fps, FALSE);
        {
            const HWND combo = GetDlgItem(dlg, IDC_STREAM_PRESET);
            const struct { EncoderPreset preset; const wchar_t* label; } presets[] = {
                { EncoderPreset::Fastest,  L"Fastest: least GPU time" },
                { EncoderPreset::Balanced, L"Balanced (default)" },
                { EncoderPreset::Quality,  L"Quality: most GPU time" },
            };
            for (const auto& p : presets) {
                const LRESULT i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.label));
                SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(i), static_cast<LPARAM>(p.preset));
                if (p.preset == s.preset) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            }
        }

        const net::EncoderChoice encoder = net::ChooseEncoder();
        state->encoderAvailable = !encoder.name.empty();
        SetDlgItemTextW(dlg, IDC_STREAM_ENCODER,
                        encoder.name.empty() ? L"No H.264 encoder found: streaming is unavailable."
                        : encoder.kind == net::EncoderKind::Software
                            ? L"Encoder: the CPU (no hardware encoder found)"
                            : (L"Encoder: " + encoder.name).c_str());
        SetDlgItemTextW(dlg, IDC_STREAM_HELP,
                        s.key.empty() && !s.lockedKey.empty()
                            ? L"The saved key could not be decrypted here. Enter it again, or generate a "
                              L"new one."
                        : MinKeyChars() > 8
                            ? L"The sign-in screen service uses these same settings. Enter the same key in "
                              L"the client."
                            : L"Forward the UDP port on your router to this PC and enter the same key "
                              L"in the client. Allow the app through Windows Firewall when asked.");
        RefreshStatus(dlg, *state);
        SetTimer(dlg, kStatusTimer, 1000, nullptr);   // Client count, live.
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kStatusTimer) RefreshStatus(dlg, *state);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_STREAM_GENERATE: {
            SetDlgItemTextW(dlg, IDC_STREAM_KEY, RandomKey().c_str());
            // A new key has to be read to be typed on the other machine.
            CheckDlgButton(dlg, IDC_STREAM_SHOWKEY, BST_CHECKED);
            RevealEditText(GetDlgItem(dlg, IDC_STREAM_KEY), true);
            return TRUE;
        }

        case IDC_STREAM_SHOWKEY:
            RevealEditText(GetDlgItem(dlg, IDC_STREAM_KEY),
                           IsDlgButtonChecked(dlg, IDC_STREAM_SHOWKEY) == BST_CHECKED);
            return TRUE;

        case IDC_STREAM_TOGGLE:
            if (state->control->running()) {
                state->control->stop();
                state->settings->enabled = false;
            } else {
                // Starting commits the fields as they stand.
                StreamSettings s;
                if (!ReadFields(dlg, *state->settings, true, s)) return TRUE;
                s.enabled = true;
                std::wstring error;
                if (state->control->start(s, error)) {
                    *state->settings = s;
                } else {
                    MessageBoxW(dlg, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
                }
            }
            RefreshStatus(dlg, *state);
            return TRUE;

        case IDOK: {
            const bool running = state->control->running();
            StreamSettings s;
            if (!ReadFields(dlg, *state->settings, running, s)) return TRUE;
            s.enabled = running;
            *state->settings = s;
            KillTimer(dlg, kStatusTimer);
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            KillTimer(dlg, kStatusTimer);
            EndDialog(dlg, IDCANCEL);
            return TRUE;

        default:
            break;
        }
        break;

    case WM_DESTROY:
        if (g_openDialog == dlg) g_openDialog = nullptr;
        break;

    default:
        break;
    }
    return FALSE;
}

}  // namespace

#if RVM_LOGIN_SERVICE
bool MachineSettingsInUse() {
    HKEY key = OpenMachineSettings(KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (key) RegCloseKey(key);
    return key != nullptr;
}

bool LoadMachineStreamSettings(StreamSettings& s) {
    HKEY key = OpenMachineSettings(KEY_QUERY_VALUE);
    if (!key) return false;
    s = StreamSettings{};
    s.enabled     = ReadDword(key, L"Enabled", 0) != 0;
    s.port        = static_cast<uint16_t>(ClampI(ReadDword(key, L"Port", net::kDefaultPort), 1, 65535));
    s.bitrateKbps = static_cast<UINT>(ClampI(ReadDword(key, L"BitrateKbps", 8000), 1000, 100000));
    s.fps         = static_cast<UINT>(ClampI(ReadDword(key, L"Fps", 60), static_cast<int>(kMinStreamFps),
                                             static_cast<int>(kMaxStreamFps)));
    s.preset      = static_cast<EncoderPreset>(ClampI(ReadDword(key, L"Preset", 0), 0, 2));

    std::vector<uint8_t> blob;
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, L"KeyBlob", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS &&
        size > 0) {
        blob.resize(size);
        if (RegGetValueW(key, nullptr, L"KeyBlob", RRF_RT_REG_BINARY, nullptr, blob.data(), &size) !=
            ERROR_SUCCESS) {
            blob.clear();
        }
    }
    RegCloseKey(key);
    if (!UnprotectForMachine(blob, s.key) && !blob.empty()) {
        s.lockedKey = std::move(blob);
        Log(L"stream: the machine's saved key could not be decrypted");
    }
    return true;
}

bool SaveMachineStreamSettings(const StreamSettings& s) {
    std::vector<uint8_t> blob = s.lockedKey;
    if (!s.key.empty() && !ProtectForMachine(s.key, blob)) return false;
    HKEY key = OpenMachineSettings(KEY_SET_VALUE);
    if (!key) return false;
    const bool ok = WriteDword(key, L"Enabled", s.enabled ? 1 : 0) && WriteDword(key, L"Port", s.port) &&
                    WriteDword(key, L"BitrateKbps", s.bitrateKbps) && WriteDword(key, L"Fps", s.fps) &&
                    WriteDword(key, L"Preset", static_cast<DWORD>(s.preset)) &&
                    RegSetValueExW(key, L"KeyBlob", 0, REG_BINARY, blob.data(),
                                   static_cast<DWORD>(blob.size())) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}
#endif

StreamSettings LoadStreamSettings() {
#if RVM_LOGIN_SERVICE
    StreamSettings machine;
    if (MachineSettingsInUse() && LoadMachineStreamSettings(machine)) return machine;
#endif
    return LoadUserStreamSettings();
}

bool SaveStreamSettings(const StreamSettings& s) {
#if RVM_LOGIN_SERVICE
    if (MachineSettingsInUse()) return SaveMachineStreamSettings(s);
#endif
    return SaveUserStreamSettings(s);
}

StreamSettings LoadUserStreamSettings() {
    const std::wstring path = SettingsPath();
    StreamSettings s;
    s.enabled     = ReadInt(L"Enabled", 0, path) != 0;
    s.port        = static_cast<uint16_t>(ClampI(ReadInt(L"Port", net::kDefaultPort, path), 1, 65535));
    s.bitrateKbps = static_cast<UINT>(ClampI(ReadInt(L"BitrateKbps", 8000, path), 1000, 100000));
    s.fps         = static_cast<UINT>(ClampI(ReadInt(L"Fps", 60, path), static_cast<int>(kMinStreamFps),
                                                   static_cast<int>(kMaxStreamFps)));
    s.preset      = static_cast<EncoderPreset>(ClampI(ReadInt(L"Preset", 0, path), 0, 2));

    const std::vector<uint8_t> blob = net::FromHex(ReadStr(L"KeyBlob", path));
    std::wstring secret;
    if (net::UnprotectSecret(blob, secret)) {
        s.key = std::move(secret);
    } else if (!blob.empty()) {
        s.lockedKey = blob;
        Log(L"stream: the saved key could not be decrypted by this Windows account");
    }
    return s;
}

bool SaveUserStreamSettings(const StreamSettings& s) {
    std::vector<uint8_t> blob = s.lockedKey;
    if (!s.key.empty() && !net::ProtectSecret(s.key, blob)) return false;

    std::wstring text = L"[Stream]\r\n";
    text += L"Enabled=" + std::to_wstring(s.enabled ? 1 : 0) + L"\r\n";
    text += L"Port=" + std::to_wstring(s.port) + L"\r\n";
    text += L"BitrateKbps=" + std::to_wstring(s.bitrateKbps) + L"\r\n";
    text += L"Fps=" + std::to_wstring(s.fps) + L"\r\n";
    text += L"Preset=" + std::to_wstring(static_cast<int>(s.preset)) + L"\r\n";
    text += L"KeyBlob=" + net::ToHex(blob) + L"\r\n";
    return WriteTextAtomically(SettingsPath(), text);
}

bool ShowStreamSettingsDialog(HWND owner, StreamSettings& settings, const StreamControl& control) {
    if (g_openDialog) {
        SetForegroundWindow(g_openDialog);
        return false;
    }
    DialogState state{ &settings, &control };
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_STREAM), owner,
                           &StreamDlgProc, reinterpret_cast<LPARAM>(&state)) == IDOK;
}

}  // namespace rvm
