#include "persist.h"

#include <shlobj.h>

namespace rvm {

namespace {

constexpr int kMaxMirrors = 32;
constexpr int kMaxValueChars = 256;
constexpr int kMaxCoord = 1 << 20;   // Screen positions; extents use kMaxExtent.
constexpr int kScreenMargin = 24;

// A match needs more than the executable: exe + class, or exe + title. A bare
// exe-only guess would bind a saved mirror to whichever of the app's windows
// happens to be frontmost.
constexpr int kMinMatchScore = 3;

// Window titles are chosen by other applications and land in a line-based
// format, where an embedded newline would forge extra keys or whole sections.
std::wstring SanitizeValue(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](wchar_t c) { return c < 0x20 || c == 0x7F; }),
            s.end());
    if (s.size() > kMaxValueChars) s.resize(kMaxValueChars);
    return s;
}

std::wstring ReadStr(const std::wstring& section, const std::wstring& key,
                     const std::wstring& path) {
    wchar_t buffer[kMaxValueChars + 1]{};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", buffer,
                             ARRAYSIZE(buffer), path.c_str());
    return buffer;
}

int ReadInt(const std::wstring& section, const std::wstring& key, int fallback,
            const std::wstring& path) {
    return static_cast<int>(
        GetPrivateProfileIntW(section.c_str(), key.c_str(), fallback, path.c_str()));
}

int ReadCoord(const std::wstring& section, const std::wstring& key,
              const std::wstring& path) {
    return ClampI(ReadInt(section, key, 0, path), -kMaxCoord, kMaxCoord);
}

int ReadExtent(const std::wstring& section, const std::wstring& key,
               const std::wstring& path) {
    return ClampI(ReadInt(section, key, 0, path), 0, kMaxExtent);
}

// String values are quoted: GetPrivateProfileString strips one pair of quotes,
// but trims unquoted leading and trailing spaces, which would alter a title.
void Line(std::wstring& out, const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L"=\"";
    out += SanitizeValue(value);
    out += L"\"\r\n";
}

void Line(std::wstring& out, const wchar_t* key, int value) {
    out += key;
    out += L'=';
    out += std::to_wstring(value);
    out += L"\r\n";
}

}  // namespace

bool WriteTextAtomically(const std::wstring& path, const std::wstring& text) {
    // Unique per process and thread, so two writers cannot trample one temp file.
    const std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                             std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const bool ok =
        WriteFile(file, &bom, sizeof(bom), &written, nullptr) &&
        WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                  &written, nullptr) &&
        FlushFileBuffers(file);
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

namespace {

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ExeNameForPid(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    // Long-path installs exceed MAX_PATH; the NT limit is 32767 characters.
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!ok) return {};

    path.resize(length);
    const size_t slash = path.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

// Titles carry volatile prefixes: a dirty marker ("*", "●") or an unread
// count ("(3) "). They are ignored when titles are compared.
std::wstring TitleCore(const std::wstring& title) {
    size_t i = 0;
    const auto skipSpace = [&] { while (i < title.size() && iswspace(title[i])) ++i; };
    skipSpace();
    while (i < title.size() && (title[i] == L'*' || title[i] == L'\x25CF' || title[i] == L'\x2022')) {
        ++i;
        skipSpace();
    }
    if (i < title.size() && title[i] == L'(') {
        size_t j = i + 1;
        while (j < title.size() && iswdigit(title[j])) ++j;
        if (j > i + 1 && j < title.size() && title[j] == L')') {
            i = j + 1;
            skipSpace();
        }
    }
    return title.substr(i);
}

struct MatchState {
    const MirrorState* want;
    const std::vector<HWND>* exclude;
    std::vector<std::pair<DWORD, std::wstring>> exeByPid;   // One lookup per process.
    HWND best  = nullptr;
    int  score = 0;
    bool bestHasTitle = false;
    int  tiedWithoutTitle = 0;   // Windows at the best score with no title evidence.

    const std::wstring& ExeFor(HWND hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        for (const auto& [p, exe] : exeByPid) {
            if (p == pid) return exe;
        }
        exeByPid.emplace_back(pid, ExeNameForPid(pid));
        return exeByPid.back().second;
    }
};

BOOL CALLBACK MatchProc(HWND hwnd, LPARAM lp) {
    auto* st = reinterpret_cast<MatchState*>(lp);
    const MirrorState& want = *st->want;

    if (std::find(st->exclude->begin(), st->exclude->end(), hwnd) != st->exclude->end()) return TRUE;
    if (!IsCapturableWindow(hwnd)) return TRUE;
    if (!want.exeName.empty() && !EqualsNoCase(st->ExeFor(hwnd), want.exeName)) return TRUE;

    int score = 1;
    if (!want.className.empty() && want.className == WindowClassName(hwnd)) score += 2;

    bool titled = false;
    if (!want.title.empty()) {
        const std::wstring title = TitleCore(WindowTitle(hwnd));
        const std::wstring wanted = TitleCore(want.title);
        if (!wanted.empty() && title == wanted) {
            score += 8;
            titled = true;
        } else {
            // A long shared beginning still counts, but only a substantial
            // one: a common app-name prefix alone is not evidence.
            const size_t shared = (std::min)(title.size(), wanted.size());
            size_t common = 0;
            while (common < shared && title[common] == wanted[common]) ++common;
            if (common >= 8 && common * 2 >= wanted.size()) {
                score += 3;
                titled = true;
            }
        }
    }

    // Without a saved executable, nothing but the title can tell apps apart.
    if (want.exeName.empty() && !titled) return TRUE;
    if (score < kMinMatchScore) return TRUE;

    if (score > st->score) {
        st->score = score;
        st->best = hwnd;
        st->bestHasTitle = titled;
        st->tiedWithoutTitle = titled ? 0 : 1;
    } else if (score == st->score && !titled && !st->bestHasTitle) {
        ++st->tiedWithoutTitle;
    }
    return TRUE;
}

BOOL CALLBACK MonitorContainsProc(HMONITOR, HDC, LPRECT, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return FALSE;
}

}  // namespace

std::wstring ConfigDir() {
    static const std::wstring dir = [] {
        // The roaming AppData folder from the shell, not the environment,
        // which can be missing or overridden.
        std::wstring d;
        PWSTR roaming = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr,
                                           &roaming))) {
            d = std::wstring(roaming) + L"\\RearViewMirror";
        }
        CoTaskMemFree(roaming);
        if (!d.empty() &&
            (CreateDirectoryW(d.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) {
            return d;
        }
        // Last resort: beside the executable.
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        d = exe;
        const size_t slash = d.find_last_of(L'\\');
        if (slash != std::wstring::npos) d.resize(slash);
        return d;
    }();
    return dir;
}

uint32_t GroupFromIdentity(const MirrorState& state) {
    // FNV-1a over the saved identity.
    uint32_t h = 2166136261u;
    for (const std::wstring* part : { &state.exeName, &state.className, &state.title }) {
        for (wchar_t c : *part) {
            h ^= static_cast<uint32_t>(towlower(c));
            h *= 16777619u;
        }
        h ^= 0x1F;   // Separator, so "ab"+"c" differs from "a"+"bc".
        h *= 16777619u;
    }
    return h == 0 ? 1 : h;
}

std::wstring ConfigPath() {
    return ConfigDir() + L"\\mirrors.ini";
}

void FillIdentity(HWND hwnd, MirrorState& state) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    state.exeName   = ExeNameForPid(pid);
    state.className = WindowClassName(hwnd);
    state.title     = WindowTitle(hwnd);
}

HWND FindMatchingWindow(const MirrorState& state, const std::vector<HWND>& exclude) {
    if (state.exeName.empty() && state.className.empty() && state.title.empty()) return nullptr;
    MatchState st{ &state, &exclude };
    EnumWindows(&MatchProc, reinterpret_cast<LPARAM>(&st));
    // Executable and class alone are enough only when they point at a single
    // window. With several candidates and no title to choose by, binding to
    // any of them could mirror, and stream, the wrong window: wait instead.
    if (st.best && !st.bestHasTitle && st.tiedWithoutTitle > 1) return nullptr;
    return st.best;
}

RECT ClampToVisibleMonitor(const RECT& rect) {
    bool onScreen = false;
    EnumDisplayMonitors(nullptr, &rect, &MonitorContainsProc, reinterpret_cast<LPARAM>(&onScreen));
    if (onScreen) return rect;

    // Its monitor is gone; drop it on the primary instead.
    const RECT work = WorkAreaFor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    RECT r = rect;
    r.left   = work.left + kScreenMargin;
    r.top    = work.top + kScreenMargin;
    r.right  = r.left + RectW(rect);
    r.bottom = r.top + RectH(rect);
    return r;
}

std::vector<MirrorState> LoadMirrorStates() {
    const std::wstring path = ConfigPath();
    std::vector<MirrorState> states;

    const int count = ClampI(ReadInt(L"General", L"Count", 0, path), 0, kMaxMirrors);
    const int legacyClickThrough = ReadInt(L"General", L"ClickThroughAll", 0, path);
    for (int i = 0; i < count; ++i) {
        const std::wstring section = L"Mirror" + std::to_wstring(i);

        MirrorState s;
        s.exeName   = ReadStr(section, L"Exe", path);
        s.className = ReadStr(section, L"Class", path);
        s.title     = ReadStr(section, L"Title", path);

        s.crop.left   = ReadExtent(section, L"CropLeft", path);
        s.crop.top    = ReadExtent(section, L"CropTop", path);
        s.crop.right  = ReadExtent(section, L"CropRight", path);
        s.crop.bottom = ReadExtent(section, L"CropBottom", path);
        s.baseSize.cx = ReadExtent(section, L"BaseWidth", path);
        s.baseSize.cy = ReadExtent(section, L"BaseHeight", path);

        s.placement.left   = ReadCoord(section, L"X", path);
        s.placement.top    = ReadCoord(section, L"Y", path);
        s.placement.right  = s.placement.left + ReadExtent(section, L"W", path);
        s.placement.bottom = s.placement.top + ReadExtent(section, L"H", path);

        s.opacity      = ClampI(ReadInt(section, L"Opacity", 100, path), 5, 100) / 100.0f;
        s.aspectLocked = ReadInt(section, L"AspectLock", 1, path) != 0;
        s.track = ReadInt(section, L"Track", 0, path) == 1 ? TrackMode::Proportional
                                                          : TrackMode::Anchored;
        s.enabled = ReadInt(section, L"Enabled", 1, path) != 0;
        // Files from before click-through was per mirror carried one global flag.
        s.clickThrough = ReadInt(section, L"ClickThrough", legacyClickThrough, path) != 0;
        s.hidden       = ReadInt(section, L"Hidden", 0, path) != 0;
        s.group = static_cast<uint32_t>(wcstoul(ReadStr(section, L"Group", path).c_str(), nullptr, 10));
        if (s.group == 0) s.group = GroupFromIdentity(s);   // Saved before groups existed.

        if (RectW(s.crop) > 0 && RectH(s.crop) > 0) states.push_back(std::move(s));
    }
    return states;
}

bool SaveMirrorStates(const std::vector<MirrorState>& states) {
    const int count = (std::min)(static_cast<int>(states.size()), kMaxMirrors);

    std::wstring text;
    text += L"[General]\r\n";
    Line(text, L"Count", count);

    for (int i = 0; i < count; ++i) {
        const MirrorState& s = states[static_cast<size_t>(i)];
        text += L"[Mirror" + std::to_wstring(i) + L"]\r\n";
        Line(text, L"Exe", s.exeName);
        Line(text, L"Class", s.className);
        Line(text, L"Title", s.title);
        Line(text, L"CropLeft", s.crop.left);
        Line(text, L"CropTop", s.crop.top);
        Line(text, L"CropRight", s.crop.right);
        Line(text, L"CropBottom", s.crop.bottom);
        Line(text, L"BaseWidth", s.baseSize.cx);
        Line(text, L"BaseHeight", s.baseSize.cy);
        Line(text, L"X", s.placement.left);
        Line(text, L"Y", s.placement.top);
        Line(text, L"W", RectW(s.placement));
        Line(text, L"H", RectH(s.placement));
        Line(text, L"Opacity", static_cast<int>(std::lround(s.opacity * 100.0f)));
        Line(text, L"AspectLock", s.aspectLocked ? 1 : 0);
        Line(text, L"Track", s.track == TrackMode::Proportional ? 1 : 0);
        Line(text, L"Enabled", s.enabled ? 1 : 0);
        Line(text, L"ClickThrough", s.clickThrough ? 1 : 0);
        Line(text, L"Hidden", s.hidden ? 1 : 0);
        Line(text, L"Group", std::to_wstring(s.group));
    }

    return WriteTextAtomically(ConfigPath(), text);
}

}  // namespace rvm
