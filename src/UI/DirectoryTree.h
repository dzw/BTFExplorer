#pragma once

#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <vector>

class DirectoryTree {
public:
    HWND Create(HWND parent, int controlId, HFONT font);
    HWND Handle() const { return hwnd_; }

    void PopulateDrives();
    bool HandleNotification(const NMHDR* notification, std::wstring& selectedPath);
    void SyncToPath(const std::wstring& path, bool showErrors);

    // 中键点击树节点（TreeView 没有中键通知，子类过程捕获后回调）
    void SetMiddleClickCallback(std::function<void(const std::wstring&)> callback);

    // 收集所有已展开节点的目录路径（外部变化监视用）
    void CollectExpandedPaths(std::vector<std::wstring>& out);
    // 外部变化后刷新某目录对应的树节点：已展开则重新枚举子目录，
    // 未展开则丢弃已填充的子项让下次展开重新枚举。不触发导航回调。
    void RefreshNode(const std::wstring& dir);

    // 节点展开/收起后回调（UI 线程），用于更新外部变化监视集
    std::function<void()> expandedChanged;

private:
    static LRESULT CALLBACK TreeProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT TreeProc(HWND, UINT, WPARAM, LPARAM);
    void ExpandNode(HTREEITEM item);
    HTREEITEM FindNodeByPath(HTREEITEM from, const std::wstring& path);
    void CollectExpandedRecursive(HTREEITEM parent, std::vector<std::wstring>& out);

    HWND hwnd_ = nullptr;
    bool syncingSelection_ = false;
    WNDPROC defaultProc_ = nullptr;
    std::function<void(const std::wstring&)> onMiddleClick_;
};
