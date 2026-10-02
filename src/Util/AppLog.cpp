#include "AppLog.h"
#include <windows.h>
#include <string>
#include <vector>

namespace {
std::wstring GetWritableLogPath(const std::wstring& root)
{
    if (root.empty()) return {};
    std::wstring directory = root + L"\\PagedExplorer";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory + L"\\PagedExplorer.log";
}

std::wstring GetLogPath()
{
    std::vector<wchar_t> localAppData(32768);
    DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    if (length > 0 && length < localAppData.size()) {
        std::wstring path = GetWritableLogPath(std::wstring(localAppData.data(), length));
        if (!path.empty()) return path;
    }

    std::vector<wchar_t> tempPath(32768);
    length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
    if (length == 0 || length >= tempPath.size()) return {};
    std::wstring path = GetWritableLogPath(std::wstring(tempPath.data(), length));
    if (path.empty()) OutputDebugStringW(L"PagedExplorer: unable to create application log file.\n");
    return path;
}
}

void WriteAppLog(const wchar_t* event)
{
    static const std::wstring logPath = GetLogPath();
    if (logPath.empty()) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestamp[64]{};
    swprintf_s(timestamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds);

    std::wstring line = timestamp;
    line += L" [pid=" + std::to_wstring(GetCurrentProcessId()) + L"] ";
    line += event ? event : L"(null)";
    line += L"\r\n";

    int byteCount = WideCharToMultiByte(CP_UTF8, 0, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        OutputDebugStringW(L"PagedExplorer: failed to convert log entry to UTF-8.\n");
        return;
    }
    std::string utf8(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
            utf8.data(), byteCount, nullptr, nullptr) != byteCount) {
        OutputDebugStringW(L"PagedExplorer: failed to convert log entry to UTF-8.\n");
        return;
    }

    HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        OutputDebugStringW(L"PagedExplorer: failed to open application log file.\n");
        return;
    }
    DWORD written = 0;
    if (!WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) ||
        written != utf8.size()) {
        OutputDebugStringW(L"PagedExplorer: failed to write application log entry.\n");
    }
    CloseHandle(file);
}
