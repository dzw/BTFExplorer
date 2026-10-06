#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <functional>
#include <string>
#include <vector>
#include "../Shell/ShellUtil.h"

// Q-Dir 同款的第二种文件列表实现：把 shell 自带的文件夹视图
// （IShellView::CreateViewWindow，窗口类 SHELLDLL_DefView/FolderView——
// 与资源管理器同一份实现）嵌进窗格。排序、图标、右键菜单、拖放、键盘
// 全是资源管理器原生行为；数据不经过本程序的分页模型，由视图自己枚举。
//
// 生命周期：MainWindow 经 unique_ptr 持有（普通 C++ 对象）；shell 视图内部
// 会 AddRef/Release 本对象（IShellBrowser），Release 只递减不自杀，
// 避免 COM 引用与 C++ 所有权互相纠缠。销毁顺序：Destroy() 先解绑视图。
class ShellFolderView : public IShellBrowser {
public:
    ShellFolderView() = default;
    ~ShellFolderView();

    bool Create(HWND parent);        // 宿主窗口（视图在首次 Navigate 时填入）
    HWND Host() const { return host_; }
    HWND ViewWindow() const { return view_; }
    bool HasView() const { return viewObj_.Get() != nullptr; }
    const std::wstring& Directory() const { return dir_; }

    // 导航到目录：销毁旧视图重建（shell 视图自己异步枚举、显示加载进度）
    bool Navigate(const std::wstring& dir);
    void Destroy();                  // 解绑并销毁视图（切回自绘列表/窗格销毁时）
    void Layout();                   // 宿主尺寸变化后让视图填满客户区

    // 视图内双击文件夹/上级 -> 目标目录路径（ BrowseObject 触发；
    // 接收方必须延后处理：Navigate 会销毁发起回调的这个视图）
    std::function<void(const std::wstring&)> onBrowse;
    // 视图发来的状态栏文本（“N 个对象”等）
    std::function<void(const std::wstring&)> onStatusText;

    // 选中项的文件系统路径（虚拟位置跳过）
    std::vector<std::wstring> SelectedPaths() const;

    // --- IUnknown ---
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    // --- IOleWindow ---
    IFACEMETHODIMP GetWindow(HWND* phwnd) override;
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    // --- IShellBrowser ---
    IFACEMETHODIMP InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetMenuSB(HMENU, HOLEMENU, HWND) override { return E_NOTIMPL; }
    IFACEMETHODIMP RemoveMenusSB(HMENU) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStatusTextSB(LPCWSTR pszText) override;
    IFACEMETHODIMP EnableModelessSB(BOOL) override { return S_OK; }
    IFACEMETHODIMP TranslateAcceleratorSB(LPMSG, WORD) override { return S_FALSE; }
    IFACEMETHODIMP BrowseObject(PCIDLIST_ABSOLUTE pidl, UINT flags) override;
    IFACEMETHODIMP GetViewStateStream(DWORD, IStream**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetControlWindow(UINT, HWND*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryActiveShellView(IShellView**) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnViewWindowActive(IShellView*) override { return S_OK; }
    IFACEMETHODIMP SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return E_NOTIMPL; }

private:
    static LRESULT CALLBACK HostProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT HostProc(HWND, UINT, WPARAM, LPARAM);

    HWND host_ = nullptr;            // 宿主窗口（窗格 tab 的子窗口，布局定位用）
    HWND view_ = nullptr;            // shell 视图窗口（SHELLDLL_DefView）
    ComPtr<IShellView> viewObj_;
    std::wstring dir_;
    UniquePIDL dirPidl_;
    ULONG refs_ = 1;                 // 归属引用在 MainWindow；Release 不 delete
};
