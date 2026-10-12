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

    // 视图内激活文件夹（双击/回车）或请求上级 -> 目标目录路径（onBrowse 触发；
    // 接收方必须延后处理：Navigate 会销毁发起回调的这个视图）
    std::function<void(const std::wstring&)> onBrowse;
    // 视图发来的状态栏文本（“N 个对象”等）
    std::function<void(const std::wstring&)> onStatusText;
    // 视图内右键（鼠标右键/菜单键）：shell 的原生菜单不带本程序的自定义项
    //（复制名称/全路径、添加到收藏、过滤后的菜单），所以拦下 WM_CONTEXTMENU
    // 交回调，由主窗口弹同一套菜单。接收方必须延后处理（回调在视图消息链内）。
    std::function<void(POINT screenPt)> onContextMenu;
    // 视图内按 F2：shell 的就地改名要走它自己那套浏览器接口（本站点只实现了
    // IShellBrowser 的最小集，多数取不到编辑框），拦下来交回调，由主窗口用与
    // 自绘列表相同的重命名对话框处理。接收方同样要延后处理。
    std::function<void()> onRenameKey;
    // 视图内真正的用户输入（鼠标按键 / 键盘）：请求宿主把本视图所在窗格设为
    // 激活窗格。shell 视图的消息不走自绘列表那条 FileList::Proc，所以点它不会
    // 像自绘列表那样自动激活，得由这条回调补上（两者对等）。
    // 接收方必须延后处理：回调发生在 shell 视图自己的消息链内，直接切换会重入。
    std::function<void()> onActivateRequest;

    // 选中项的文件系统路径（虚拟位置跳过）
    std::vector<std::wstring> SelectedPaths() const;

    // 光标（屏幕坐标）所在条目的文件系统路径；空白处返回空。
    // outOnItem 回填“光标是否落在某个条目上”（取不到路径时也为 true，
    // 用于区分“点了空白”与“点了条目但条目是虚拟位置”）。
    std::wstring ItemPathAt(POINT screenPt, bool* outOnItem) const;

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

    // 视图窗口（SHELLDLL_DefView）子类化：内层 SysListView32 激活文件夹时把
    // NM_DBLCLK/NM_RETURN 发到本视图窗口，shell 默认会 ShellExecute 出新
    // 资源管理器窗口，并不会调用本站点的 IShellBrowser::BrowseObject。
    // 在这里拦下“选中项是文件夹”的激活通知，转成站内导航（onBrowse），
    // 不转给原过程即可阻止新窗口；其余消息一律原样转交。
    static LRESULT CALLBACK ViewProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT ViewProc(HWND, UINT, WPARAM, LPARAM);
    void InstallViewSubclass();          // 每次 Navigate 新建视图后安装

    // 右键/改名按键拦截：WM_CONTEXTMENU 可能落在视图窗口本身，也可能落在它内部的
    // SysListView32（shell 在自己的列表上处理菜单），两层都装子类化才不漏。
    // 谁先收到谁回调，ctxTick_ 记下时刻，200ms 内的同一次右键不再重复弹。
    static LRESULT CALLBACK CtxProcStatic(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    bool TakeContextMenu(LPARAM lp);
    bool TakeRenameKey();
    bool InstallContextMenuSubclass();   // 返回内部 SysListView32 是否已挂上
    void RemoveContextMenuSubclass();

    HWND InnerListView() const;          // 视图内部的 SysListView32（找不到返回空）

    std::wstring ActivatedFolderPath() const;  // 当前选中项若是文件夹返回其绝对路径

    HWND host_ = nullptr;            // 宿主窗口（窗格 tab 的子窗口，布局定位用）
    HWND view_ = nullptr;            // shell 视图窗口（SHELLDLL_DefView）
    WNDPROC origViewProc_ = nullptr; // 视图窗口原窗口过程（子类化前）
    HWND subclassedOn_ = nullptr;    // 子类化实际装在该 HWND 上；沿用同窗口时不重装
    std::vector<HWND> ctxSubs_;      // 装了按键/右键子类化的窗口（视图窗口 + 内部列表）
    DWORD ctxTick_ = 0;              // 最近一次右键拦截的时刻（去重用）
    HWND ctxList_ = nullptr;         // 已挂拦截的内部列表；shell 重建它后句柄会变
    ComPtr<IShellView> viewObj_;
    std::wstring dir_;
    UniquePIDL dirPidl_;
    ULONG refs_ = 1;                 // 归属引用在 MainWindow；Release 不 delete
};
