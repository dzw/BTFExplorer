#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

// 目录变化监视器：单线程监视一组目录（FindFirstChangeNotification +
// WaitForMultipleObjects，上限 62 个），任一目录内文件/子目录变化时向
// 指定窗口投递 msg（wParam = new std::wstring*，接收方负责 delete）。
// 变化带 400ms 防抖（复制大文件时连续信号合并为一次通知）。
// 监视集由 UI 线程通过 SetDirectories 随时更新（去重后的绝对路径）。
class DirWatcher {
public:
    void Start(HWND hwnd, UINT msg);
    void Stop();
    void SetDirectories(std::vector<std::wstring> dirs);   // 任意线程可调

    DirWatcher() = default;
    ~DirWatcher() { Stop(); }
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

private:
    void Loop();

    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    HANDLE wake_ = nullptr;          // SetDirectories 用它立刻唤醒监视线程
    std::thread thread_;
    std::mutex mtx_;
    std::vector<std::wstring> want_; // UI 线程请求的监视集
    bool stop_ = false;
};
