#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <functional>
#include <string>
#include "../Shell/ShellUtil.h"

// 左侧目录树：封装 shell 自带的“命名空间树控件”（CLSID_NamespaceTreeControl，
// Q-Dir 同款做法）。树的节点、图标、Infotip、外部变化后的自动刷新全部由 shell
// 维护，本类只负责三件事：设置根（命名空间桌面）、把当前目录同步为选中项、
// 把选中/中键点击转成文件系统路径回调给上层导航。
class DirectoryTree {
public:
    DirectoryTree() = default;
    ~DirectoryTree();
    DirectoryTree(const DirectoryTree&) = delete;
    DirectoryTree& operator=(const DirectoryTree&) = delete;

    bool Create(HWND parent);   // parent = 左侧 Tab 容器，控件窗口成为其子窗口
    HWND Handle() const { return hwnd_; }

    // 选中变化回调（用户点击/键盘导航；在控件消息链内触发，接收方应延后处理）。
    // 仅文件系统位置回调；虚拟位置（此电脑/回收站等）没有路径，不回调。
    void SetSelectionCallback(std::function<void(const std::wstring&)> cb) { onSelection_ = std::move(cb); }
    // 中键点击节点回调（同上，延后处理）
    void SetMiddleClickCallback(std::function<void(const std::wstring&)> cb) { onMiddleClick_ = std::move(cb); }

    // 把目录同步为树的选中项：逐级展开祖先 + 选中 + 滚动到可见。
    // showErrors 参数保留以兼容旧调用点；控件覆盖整个命名空间，找不到时静默保持现状。
    void SyncToPath(const std::wstring& path, bool showErrors);

private:
    class EventSink;
    friend class EventSink;
    void OnSelectionChanged(IShellItem* psi);
    void OnItemClick(IShellItem* psi, NSTCEHITTEST hitTest, NSTCECLICKTYPE clickType);
    static std::wstring PathOfItem(IShellItem* psi);   // 虚拟位置返回空串

    HWND hwnd_ = nullptr;                    // 控件窗口（窗口类 "NamespaceTreeControl"）
    ComPtr<INameSpaceTreeControl2> ctl_;
    EventSink* sink_ = nullptr;              // TreeAdvise 后由控件持有引用
    DWORD adviseCookie_ = 0;
    bool syncing_ = false;                   // SyncToPath 触发的选择事件不当作用户操作
    std::function<void(const std::wstring&)> onSelection_;
    std::function<void(const std::wstring&)> onMiddleClick_;
};
