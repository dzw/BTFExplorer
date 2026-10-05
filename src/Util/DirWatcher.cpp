#include "DirWatcher.h"
#include "../Util/AppLog.h"
#include <algorithm>

// 单次最多等待的句柄数：62 个目录 + 1 个唤醒事件（MAXIMUM_WAIT_OBJECTS=64 留余量）
static constexpr int kMaxWatched = 62;
static constexpr DWORD kSettleMs = 400;   // 信号后的防抖等待
static constexpr DWORD kPollMs = 400;     // 无信号时的轮询间隔（顺带响应监视集更新）

void DirWatcher::Start(HWND hwnd, UINT msg)
{
    if (thread_.joinable()) return;
    hwnd_ = hwnd;
    msg_ = msg;
    if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = false;
    thread_ = std::thread(&DirWatcher::Loop, this);
    WriteAppLog((L"DIRWATCH started hwnd=0x" +
                 std::to_wstring(reinterpret_cast<unsigned long long>(hwnd_)) +
                 L" msg=0x" + std::to_wstring(msg_)).c_str());
}

void DirWatcher::Stop()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
}

void DirWatcher::SetDirectories(std::vector<std::wstring> dirs)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        want_ = std::move(dirs);
    }
    if (wake_) SetEvent(wake_);
}

void DirWatcher::Loop()
{
    std::vector<std::wstring> current;   // 当前实际监视的目录
    std::vector<HANDLE> handles;         // 与 current 一一对应

    auto closeAll = [&]() {
        for (HANDLE h : handles) FindCloseChangeNotification(h);
        handles.clear();
        current.clear();
    };

    for (;;) {
        // 取最新监视集；有变化就重建句柄
        std::vector<std::wstring> want;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) { closeAll(); return; }
            want = want_;
        }
        bool same = (want.size() == current.size());
        if (same) {
            for (size_t i = 0; i < want.size(); ++i)
                if (_wcsicmp(want[i].c_str(), current[i].c_str()) != 0) { same = false; break; }
        }
        if (!same) {
            WriteAppLog((L"DIRWATCH rebuild n=" + std::to_wstring(want.size())).c_str());
            closeAll();
            size_t n = std::min<size_t>(want.size(), kMaxWatched);
            for (size_t i = 0; i < n; ++i) {
                HANDLE h = FindFirstChangeNotificationW(want[i].c_str(), FALSE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
                if (h != INVALID_HANDLE_VALUE) {
                    handles.push_back(h);
                    current.push_back(want[i]);
                }
            }
        }

        if (handles.empty()) {
            // 没有可监视的目录：睡一小段后重新取监视集
            Sleep(kPollMs);
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) { return; }
            continue;
        }

        std::vector<HANDLE> waits = handles;
        waits.push_back(wake_);
        DWORD w = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, kPollMs);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) { closeAll(); return; }
        }
        if (w == WAIT_OBJECT_0 + waits.size() - 1) {
            continue;   // 唤醒事件：监视集有更新，回到顶部重建
        }
        if (w == WAIT_FAILED) {
            closeAll(); // 某个句柄失效（如 U 盘拔出），下一轮全部重建
            continue;
        }
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + handles.size()) {
            size_t idx = w - WAIT_OBJECT_0;
            Sleep(kSettleMs);   // 防抖：等变化稳定下来再通知
            FindNextChangeNotification(handles[idx]);
            if (!current.empty() && idx < current.size()) {
                BOOL posted = PostMessageW(hwnd_, msg_, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(current[idx])));
                WriteAppLog((L"DIRWATCH change " + current[idx] +
                             (posted ? L" posted" : L" POST-FAILED err=" +
                              std::to_wstring(GetLastError()))).c_str());
            }
        }
    }
}
