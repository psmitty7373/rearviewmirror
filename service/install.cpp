#include "login_config.h"
#include "service.h"

#include <cstdio>
#include <netfw.h>
#include <shlobj.h>

namespace rvm::login {

namespace {

constexpr wchar_t kExeName[] = L"RearViewMirrorService.exe";
constexpr wchar_t kPdbName[] = L"RearViewMirrorService.pdb";
constexpr wchar_t kFirewallRule[] = L"Rear View Mirror sign-in screen";

struct Bstr {
    BSTR s;
    explicit Bstr(const std::wstring& text) : s(SysAllocString(text.c_str())) {}
    ~Bstr() { SysFreeString(s); }
};

// Windows Firewall would ask before letting the app's port in, but nobody can
// answer at the sign-in screen: it would simply drop every client. So the
// rule is made here, as narrow as it can be: this program, inbound UDP, this
// port, private and domain networks only. Replaces any earlier one.
bool SetFirewallRule(const std::wstring* program, uint16_t port) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        winrt::com_ptr<INetFwPolicy2> policy;
        winrt::com_ptr<INetFwRules> rules;
        if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(INetFwPolicy2), policy.put_void())) &&
            SUCCEEDED(policy->get_Rules(rules.put()))) {
            const Bstr name(kFirewallRule);
            // Remove fails when there is none, which is fine. Twice: an
            // earlier install may have left more than one by that name.
            rules->Remove(name.s);
            rules->Remove(name.s);
            ok = true;
            if (program) {
                winrt::com_ptr<INetFwRule> rule;
                const Bstr app(*program), ports(std::to_wstring(port)),
                    description(L"Lets Rear View Mirror clients reach the sign-in screen while nobody is "
                                L"signed in.");
                ok = SUCCEEDED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                                __uuidof(INetFwRule), rule.put_void())) &&
                     SUCCEEDED(rule->put_Name(name.s)) &&
                     SUCCEEDED(rule->put_Description(description.s)) &&
                     SUCCEEDED(rule->put_ApplicationName(app.s)) &&
                     SUCCEEDED(rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP)) &&
                     SUCCEEDED(rule->put_LocalPorts(ports.s)) &&
                     SUCCEEDED(rule->put_Direction(NET_FW_RULE_DIR_IN)) &&
                     SUCCEEDED(rule->put_Action(NET_FW_ACTION_ALLOW)) &&
                     SUCCEEDED(rule->put_Profiles(NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN)) &&
                     SUCCEEDED(rule->put_Enabled(VARIANT_TRUE)) &&
                     SUCCEEDED(rules->Add(rule.get()));
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    if (token) CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// Program Files, where only administrators can write: a service running as
// SYSTEM from a folder its user could change would hand that user SYSTEM.
std::wstring InstallDir() {
    std::wstring dir;
    PWSTR programFiles = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        dir = std::wstring(programFiles) + L"\\RearViewMirror";
    }
    CoTaskMemFree(programFiles);
    return dir;
}

std::wstring SelfDir() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    return dir;
}

bool StopAndWait(SC_HANDLE service) {
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        return false;
    }
    for (int i = 0; i < 60; ++i) {
        if (QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(250);
    }
    return false;
}

// Deleted now, or at the next restart if it is in use.
void DeleteFileOrLater(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!DeleteFileW(path.c_str())) MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

int Fail(const wchar_t* what) {
    fwprintf(stderr, L"%s (error %lu)\n", what, GetLastError());
    return 1;
}

}  // namespace

int RunInstall() {
    if (!IsElevated()) {
        fwprintf(stderr, L"Run this from an administrator (elevated) prompt.\n");
        return 1;
    }
    // The current user's streaming settings: the sign-in screen is served on
    // the same port with the same key, so a client sees one server.
    const StreamSettings user = LoadStreamSettings();
    if (user.key.size() < kMinLoginKeyChars) {
        fwprintf(stderr, L"This account's streaming key is missing or shorter than %zu characters: too weak to "
                         L"guard the sign-in screen. Press Generate in Rear View Mirror's Streaming dialog, then "
                         L"run --install again.\n",
                 kMinLoginKeyChars);
        return 1;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return Fail(L"Cannot open the service manager.");
    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (service) {
        wprintf(L"Stopping the installed service...\n");
        if (!StopAndWait(service)) wprintf(L"  (it did not stop cleanly; carrying on)\n");
    }

    const std::wstring dir = InstallDir();
    const std::wstring exe = dir + L"\\" + kExeName;
    if (dir.empty() || (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) {
        return Fail(L"Cannot create the install folder.");
    }
    if (_wcsicmp(SelfDir().c_str(), dir.c_str()) != 0) {
        if (!CopyFileW((SelfDir() + L"\\" + kExeName).c_str(), exe.c_str(), FALSE)) {
            return Fail(L"Cannot copy the service into Program Files.");
        }
        // Symbols, so a crash report names functions; optional.
        CopyFileW((SelfDir() + L"\\" + kPdbName).c_str(), (dir + L"\\" + kPdbName).c_str(), FALSE);
    }

    std::wstring movedAside;
    if (!SecureMachineDir(movedAside)) return Fail(L"Cannot create the settings folder in ProgramData.");
    if (!movedAside.empty()) {
        wprintf(L"A folder someone else made was in the way; it is now %s\n", movedAside.c_str());
    }
    if (!SaveLoginSettings(user)) return Fail(L"Cannot save the sign-in settings.");
    const bool firewall = SetFirewallRule(&exe, user.port);

    const std::wstring command = L"\"" + exe + L"\" --service";
    if (service) {
        if (!ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                  kServiceDisplayName)) {
            return Fail(L"Cannot update the service.");
        }
    } else {
        // LocalSystem (no account given): it has to start processes in other sessions.
        service = CreateServiceW(scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service) return Fail(L"Cannot create the service.");
    }
    SERVICE_DESCRIPTIONW description{ const_cast<wchar_t*>(kServiceDescription) };
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    SC_ACTION restart[] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW failure{ 24 * 60 * 60, nullptr, nullptr, ARRAYSIZE(restart), restart };
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        return Fail(L"Installed, but the service did not start.");
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    wprintf(L"Installed and running: %s\n\n", exe.c_str());
    wprintf(L"While nobody is signed in, the sign-in screen streams on UDP port %u with this\n"
            L"account's key. A client sees the same server, listing \"Sign-in screen\".\n\n",
            user.port);
    if (firewall) {
        wprintf(L"- Windows Firewall lets UDP %u in to it, on private and domain networks.\n", user.port);
    } else {
        wprintf(L"- Could not add the Windows Firewall rule: clients will be blocked until UDP %u is\n"
                L"  allowed in for %s.\n", user.port, exe.c_str());
    }
    wprintf(L"- Rear View Mirror must start when you sign in (a shortcut in shell:startup)\n"
            L"  to take over from there.\n"
            L"- After changing the port or key in Rear View Mirror, run --install again.\n");
#if !RVM_REMOTE_CONTROL
    wprintf(L"- This build has no desktop control: the sign-in screen can be watched, not used.\n"
            L"  Build with --remote-control to sign in through it.\n");
#endif
    return 0;
}

int RunUninstall() {
    if (!IsElevated()) {
        fwprintf(stderr, L"Run this from an administrator (elevated) prompt.\n");
        return 1;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return Fail(L"Cannot open the service manager.");
    if (SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)) {
        StopAndWait(service);
        if (!DeleteService(service)) return Fail(L"Cannot remove the service.");
        CloseServiceHandle(service);
        wprintf(L"Service removed.\n");
    } else {
        wprintf(L"The service was not installed.\n");
    }
    CloseServiceHandle(scm);
    SetFirewallRule(nullptr, 0);

    const std::wstring dir = InstallDir();
    if (!dir.empty()) {
        DeleteFileOrLater(dir + L"\\" + kExeName);
        DeleteFileOrLater(dir + L"\\" + kPdbName);
        RemoveDirectoryW(dir.c_str());   // Only if nothing else is in it.
    }
    // The settings, with the key, and the logs, if the folder is ours: one
    // someone else made, or a link, is no place to delete things in.
    const std::wstring data = MachineDir();
    if (!data.empty() && MachineDirTrusted()) {
        WIN32_FIND_DATAW found{};
        HANDLE find = FindFirstFileW((data + L"\\*").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DeleteFileOrLater(data + L"\\" + found.cFileName);
                }
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }
        RemoveDirectoryW(data.c_str());
    }
    wprintf(L"Files, settings, logs and the firewall rule removed.\n");
    return 0;
}

}  // namespace rvm::login
