#pragma once

#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>

class DirectoryTree {
public:
    HWND Create(HWND parent, int controlId, HFONT font);
    HWND Handle() const { return hwnd_; }

    void PopulateDrives();
    bool HandleNotification(const NMHDR* notification, std::wstring& selectedPath);
    void SyncToPath(const std::wstring& path, bool showErrors);

    // 中键点击树节点（TreeView 没有中键通知，子类过程捕获后回调）
    void SetMiddleClickCallback(std::function<void(const std::wstring&)> callback);

private:
    static LRESULT CALLBACK TreeProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT TreeProc(HWND, UINT, WPARAM, LPARAM);
    void ExpandNode(HTREEITEM item);

    HWND hwnd_ = nullptr;
    bool syncingSelection_ = false;
    WNDPROC defaultProc_ = nullptr;
    std::function<void(const std::wstring&)> onMiddleClick_;
};
