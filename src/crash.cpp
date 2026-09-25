#include "crash.h"
#include "persist.h"

#include <dbghelp.h>

namespace rvm {

namespace {

wchar_t g_name[32] = L"app";
std::atomic<bool> g_handling{ false };

struct CrashJob {
    EXCEPTION_POINTERS* info;
    DWORD threadId;
};

// "module+0xOFFSET  function+0xDISP  file:line", as much as can be found.
std::wstring Describe(HANDLE process, DWORD64 address) {
    wchar_t text[768]{};
    HMODULE module = nullptr;
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath))) {
        const wchar_t* slash = wcsrchr(modulePath, L'\\');
        _snwprintf_s(text, _TRUNCATE, L"%s+0x%llX", slash ? slash + 1 : modulePath,
                     static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        _snwprintf_s(text, _TRUNCATE, L"0x%llX", static_cast<unsigned long long>(address));
    }
    std::wstring out = text;

    alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddrW(process, address, &displacement, symbol)) {
        _snwprintf_s(text, _TRUNCATE, L"  %s+0x%llX", symbol->Name,
                     static_cast<unsigned long long>(displacement));
        out += text;
    }
    IMAGEHLP_LINEW64 line{ sizeof(line) };
    DWORD column = 0;
    if (SymGetLineFromAddrW64(process, address, &column, &line) && line.FileName) {
        const wchar_t* slash = wcsrchr(line.FileName, L'\\');
        _snwprintf_s(text, _TRUNCATE, L"  %s:%lu", slash ? slash + 1 : line.FileName, line.LineNumber);
        out += text;
    }
    return out;
}

// Runs on a thread of its own: the crashing thread's stack may be the very
// thing that is broken.
DWORD WINAPI WriteReport(void* param) {
    const auto* job = static_cast<const CrashJob*>(param);
    const HANDLE process = GetCurrentProcess();
    const EXCEPTION_RECORD* record = job->info->ExceptionRecord;

    // Symbols from the .pdb beside the .exe, when it was copied along.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = L'\0';
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    const bool symbols = SymInitializeW(process, exe, TRUE) != FALSE;

    Log(L"CRASH: exception 0x%08X on thread %lu at %s", static_cast<unsigned>(record->ExceptionCode),
        job->threadId,
        Describe(process, reinterpret_cast<DWORD64>(record->ExceptionAddress)).c_str());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        Log(L"CRASH: %s address 0x%llX", kind == 0 ? L"reading" : kind == 1 ? L"writing" : L"executing",
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }

    // The crashing thread's stack, from the moment of the exception.
    if (symbols) {
        CONTEXT context = *job->info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset    = context.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;
        const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                         job->threadId);
        for (int i = 0; i < 64; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            Log(L"CRASH:   #%02d %s", i, Describe(process, frame.AddrPC.Offset).c_str());
        }
        if (thread) CloseHandle(thread);
    }

    // Every thread's stack, for what the log cannot show.
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[32]{};
    _snwprintf_s(stamp, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
    const std::wstring path = ConfigDir() + L"\\" + g_name + L"-crash-" + stamp + L".dmp";
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{ job->threadId, job->info, FALSE };
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                     MiniDumpWithIndirectlyReferencedMemory);
        const BOOL written = MiniDumpWriteDump(process, GetCurrentProcessId(), file, type, &exception,
                                               nullptr, nullptr);
        CloseHandle(file);
        Log(L"CRASH: %s %s", written ? L"dump written to" : L"dump failed:", path.c_str());
    }
    if (symbols) SymCleanup(process);
    return 0;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    if (g_handling.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;   // Once.
    CrashJob job{ info, GetCurrentThreadId() };
    if (HANDLE worker = CreateThread(nullptr, 1024 * 1024, &WriteReport, &job, 0, nullptr)) {
        // Bounded: if the report itself hangs (say the crash left the log's
        // lock held), the crash still goes on to Windows.
        WaitForSingleObject(worker, 30000);
        CloseHandle(worker);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // Windows reports and ends the process as before.
}

}  // namespace

void InstallCrashHandler(const wchar_t* name) {
    wcsncpy_s(g_name, name, _TRUNCATE);
    SetUnhandledExceptionFilter(&CrashFilter);
}

}  // namespace rvm
