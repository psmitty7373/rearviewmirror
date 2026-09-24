#include "connect_dialog.h"
#include "resource.h"

namespace rvm {

namespace {

std::wstring GetText(HWND dlg, int id) {
    wchar_t buffer[512]{};
    GetDlgItemTextW(dlg, id, buffer, ARRAYSIZE(buffer));
    return buffer;
}

std::wstring OneLine(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c < 0x20; }), s.end());
    return s;
}

INT_PTR CALLBACK ConnectDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* settings = reinterpret_cast<ConnectSettings*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG:
        settings = reinterpret_cast<ConnectSettings*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
        SetDlgItemTextW(dlg, IDC_CONNECT_HOST, settings->host.c_str());
        SetDlgItemInt(dlg, IDC_CONNECT_PORT, settings->port, FALSE);
        SetDlgItemTextW(dlg, IDC_CONNECT_KEY, settings->key.c_str());
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            ConnectSettings s;
            s.host = OneLine(GetText(dlg, IDC_CONNECT_HOST));
            s.port = static_cast<uint16_t>(GetDlgItemInt(dlg, IDC_CONNECT_PORT, nullptr, FALSE));
            s.key  = GetText(dlg, IDC_CONNECT_KEY);
            if (s.host.empty() || s.port == 0 || s.key.empty()) {
                MessageBoxW(dlg, L"Host, port and key are all needed.", kAppName,
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            *settings = s;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

}  // namespace

bool ShowConnectDialog(HWND owner, ConnectSettings& settings) {
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CONNECT), owner,
                           &ConnectDlgProc, reinterpret_cast<LPARAM>(&settings)) == IDOK;
}

}  // namespace rvm
