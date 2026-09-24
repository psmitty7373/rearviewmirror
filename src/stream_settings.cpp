#include "stream_settings.h"
#include "net/codec.h"
#include "net/crypto.h"
#include "persist.h"
#include "resource.h"

namespace rvm {

namespace {

std::wstring SettingsPath() {
    return ConfigDir() + L"\\stream.ini";
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
    out.key = GetText(dlg, IDC_STREAM_KEY);
    if (needKey && out.key.size() < 8) {
        MessageBoxW(dlg, L"The shared key needs at least 8 characters. Generate one, or type your own.",
                    kAppName, MB_OK | MB_ICONWARNING);
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

        const std::wstring encoder = net::HardwareEncoderName();
        state->encoderAvailable = !encoder.empty();
        SetDlgItemTextW(dlg, IDC_STREAM_ENCODER,
                        encoder.empty() ? L"No hardware H.264 encoder found: streaming is unavailable."
                                        : (L"Encoder: " + encoder).c_str());
        SetDlgItemTextW(dlg, IDC_STREAM_HELP,
                        L"Forward the UDP port on your router to this PC and enter the same key in "
                        L"the client. Allow the app through Windows Firewall when asked.");
        RefreshStatus(dlg, *state);
        SetTimer(dlg, kStatusTimer, 1000, nullptr);   // Client count, live.
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kStatusTimer) RefreshStatus(dlg, *state);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_STREAM_GENERATE:
            SetDlgItemTextW(dlg, IDC_STREAM_KEY, RandomKey().c_str());
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

StreamSettings LoadStreamSettings() {
    const std::wstring path = SettingsPath();
    StreamSettings s;
    s.enabled     = ReadInt(L"Enabled", 0, path) != 0;
    s.port        = static_cast<uint16_t>(ClampI(ReadInt(L"Port", net::kDefaultPort, path), 1, 65535));
    s.bitrateKbps = static_cast<UINT>(ClampI(ReadInt(L"BitrateKbps", 8000, path), 1000, 100000));
    s.fps         = static_cast<UINT>(ClampI(ReadInt(L"Fps", 60, path), static_cast<int>(kMinStreamFps),
                                                   static_cast<int>(kMaxStreamFps)));

    std::wstring secret;
    if (net::UnprotectSecret(net::FromHex(ReadStr(L"KeyBlob", path)), secret)) s.key = secret;
    return s;
}

bool SaveStreamSettings(const StreamSettings& s) {
    std::vector<uint8_t> blob;
    if (!s.key.empty() && !net::ProtectSecret(s.key, blob)) return false;

    std::wstring text = L"[Stream]\r\n";
    text += L"Enabled=" + std::to_wstring(s.enabled ? 1 : 0) + L"\r\n";
    text += L"Port=" + std::to_wstring(s.port) + L"\r\n";
    text += L"BitrateKbps=" + std::to_wstring(s.bitrateKbps) + L"\r\n";
    text += L"Fps=" + std::to_wstring(s.fps) + L"\r\n";
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
