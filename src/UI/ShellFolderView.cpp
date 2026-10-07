#include "ShellFolderView.h"
#include <shlwapi.h>
#include <commctrl.h>

namespace {

const wchar_t kHostClass[] = L"PEShellHostView";   // 每个窗格一个宿主窗口

} // namespace

ShellFolderView::~ShellFolderView()
{
    Destroy();
    if (host_) DestroyWindow(host_);
}

LRESULT CALLBACK ShellFolderView::HostProcStatic(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<ShellFolderView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->HostProc(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

bool ShellFolderView::Create(HWND parent)
{
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ShellFolderView::HostProcStatic;
        wc.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kHostClass;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered = true;
    }
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    host_ = CreateWindowExW(WS_EX_CLIENTEDGE, kHostClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, parent, nullptr, hInst, nullptr);
    if (!host_) return false;
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

LRESULT ShellFolderView::HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE && view_)
        MoveWindow(view_, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
    return DefWindowProcW(h, msg, wp, lp);
}

void ShellFolderView::Layout()
{
    if (!host_ || !view_) return;
    RECT rc{};
    GetClientRect(host_, &rc);
    MoveWindow(view_, 0, 0, rc.right, rc.bottom, TRUE);
}

bool ShellFolderView::Navigate(const std::wstring& dir)
{
    if (!host_ || dir.empty()) return false;
    if (dir_ == dir && viewObj_) return true;   // 已在这个目录

    UniquePIDL pidl = shell::PIDLFromPath(dir);
    if (!pidl) return false;

    Destroy();   // 简单可靠：每次导航重建视图（shell 视图自己异步加载）

    ComPtr<IShellFolder> folder;
    if (FAILED(SHBindToObject(nullptr, static_cast<PCIDLIST_ABSOLUTE>(pidl.get()),
                              nullptr, IID_PPV_ARGS(&folder))) || !folder)
        return false;
    ComPtr<IShellView> view;
    if (FAILED(folder->CreateViewObject(host_, IID_PPV_ARGS(&view))) || !view)
        return false;

    FOLDERSETTINGS fs{ FVM_DETAILS, 0 };
    RECT rc{};
    GetClientRect(host_, &rc);
    HWND viewHwnd = nullptr;
    if (FAILED(view->CreateViewWindow(nullptr, &fs, this, &rc, &viewHwnd)) || !viewHwnd)
        return false;

    viewObj_.Attach(view.Detach());
    view_ = viewHwnd;
    dir_ = dir;
    dirPidl_ = std::move(pidl);
    InstallViewSubclass();   // 拦下视图内文件夹激活，改为站内导航（见 ViewProc）
    // 以“无焦点激活”挂上浏览器：视图才会向状态栏发条目数等消息
    viewObj_->UIActivate(SVUIA_ACTIVATE_NOFOCUS);
    return true;
}

void ShellFolderView::InstallViewSubclass()
{
    if (!view_) return;
    SetWindowLongPtrW(view_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    origViewProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(view_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ShellFolderView::ViewProcStatic)));
}

LRESULT CALLBACK ShellFolderView::ViewProcStatic(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<ShellFolderView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->ViewProc(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT ShellFolderView::ViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NOTIFY && lp) {
        auto* nh = reinterpret_cast<NMHDR*>(lp);
        // 内层列表发来的“双击/回车激活”：若激活项是文件夹，站内导航并吞掉，
        // 不转给原过程即可阻止 shell 打开新的资源管理器窗口。
        if (nh->code == NM_DBLCLK || nh->code == NM_RETURN) {
            std::wstring folder = ActivatedFolderPath();
            if (!folder.empty() && onBrowse) {
                onBrowse(folder);   // 延后 PostMessage 导航（不在本视图消息链内销毁自己）
                return 1;
            }
        }
    }
    return CallWindowProcW(origViewProc_, h, msg, wp, lp);
}

std::wstring ShellFolderView::ActivatedFolderPath() const
{
    if (!viewObj_) return {};
    ComPtr<IFolderView> fv;
    if (FAILED(viewObj_->QueryInterface(IID_PPV_ARGS(&fv))) || !fv) return {};
    ComPtr<IEnumIDList> en;
    if (FAILED(fv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&en))) || !en) return {};
    PIDLIST_RELATIVE rel = nullptr;
    ULONG got = 0;
    if (en->Next(1, &rel, &got) != S_OK || got == 0 || !rel) return {};
    UniquePIDL abs(ILCombine(static_cast<PCIDLIST_ABSOLUTE>(dirPidl_.get()), rel));
    CoTaskMemFree(rel);
    wchar_t buf[MAX_PATH * 2]{};
    if (!abs || !SHGetPathFromIDListW(static_cast<PCIDLIST_ABSOLUTE>(abs.get()), buf) || !buf[0])
        return {};
    DWORD fa = GetFileAttributesW(buf);
    if (fa == INVALID_FILE_ATTRIBUTES || !(fa & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return buf;   // 虚拟位置取不到文件系统路径，返回空 -> 放行原行为
}

void ShellFolderView::Destroy()
{
    if (viewObj_) {
        viewObj_->DestroyViewWindow();
        viewObj_.Release();   // 视图析构时会 Release 本对象（IShellBrowser）
    }
    view_ = nullptr;
    dir_.clear();
    dirPidl_.reset();
}

std::vector<std::wstring> ShellFolderView::SelectedPaths() const
{
    std::vector<std::wstring> out;
    if (!viewObj_) return out;
    ComPtr<IFolderView> fv;
    if (FAILED(viewObj_->QueryInterface(IID_PPV_ARGS(&fv))) || !fv) return out;
    ComPtr<IEnumIDList> items;
    if (FAILED(fv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items))) || !items) return out;
    for (;;) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        ULONG fetched = 0;
        if (items->Next(1, &pidl, &fetched) != S_OK || fetched == 0) break;
        wchar_t buf[MAX_PATH * 2]{};
        if (SHGetPathFromIDListW(pidl, buf)) out.push_back(buf);
        CoTaskMemFree(pidl);
    }
    return out;
}

// --- IUnknown（Release 只递减不 delete，所有权在 MainWindow） ---

IFACEMETHODIMP ShellFolderView::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IOleWindow || riid == IID_IShellBrowser) {
        *ppv = static_cast<IShellBrowser*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) ShellFolderView::AddRef() { return InterlockedIncrement(&refs_); }

IFACEMETHODIMP_(ULONG) ShellFolderView::Release()
{
    ULONG remaining = InterlockedDecrement(&refs_);
    return remaining ? remaining : 1;   // 不自杀：归属权在 MainWindow 的 unique_ptr
}

// --- IOleWindow / IShellBrowser ---

IFACEMETHODIMP ShellFolderView::GetWindow(HWND* phwnd)
{
    if (!phwnd) return E_POINTER;
    *phwnd = host_;
    return host_ ? S_OK : E_FAIL;
}

IFACEMETHODIMP ShellFolderView::SetStatusTextSB(LPCWSTR pszText)
{
    if (onStatusText && pszText) onStatusText(pszText);
    return S_OK;
}

// shell 视图内双击文件夹/点上级/历史导航都会走到这里。
// 注意：本回调在视图自己的消息链内触发，接收方（主窗口）必须延后处理——
// 处理中的 Navigate 会销毁发起本次回调的视图。
IFACEMETHODIMP ShellFolderView::BrowseObject(PCIDLIST_ABSOLUTE pidl, UINT flags)
{
    if (!pidl) return E_INVALIDARG;
    std::wstring target;
    wchar_t buf[MAX_PATH * 2]{};

    if (flags & SBSP_PARENT) {
        UniquePIDL parent(ILCloneFull(static_cast<PCIDLIST_ABSOLUTE>(dirPidl_.get())));
        if (parent && ILRemoveLastID(static_cast<PIDLIST_ABSOLUTE>(parent.get())) &&
            SHGetPathFromIDListW(static_cast<PCIDLIST_ABSOLUTE>(parent.get()), buf))
            target = buf;
    } else if (flags & SBSP_RELATIVE) {
        UniquePIDL abs(ILCombine(static_cast<PCIDLIST_ABSOLUTE>(dirPidl_.get()), pidl));
        if (abs && SHGetPathFromIDListW(static_cast<PCIDLIST_ABSOLUTE>(abs.get()), buf))
            target = buf;
    } else {
        // SBSP_ABSOLUTE / SBSP_SAMEBROWSER 等都按绝对处理
        if (SHGetPathFromIDListW(pidl, buf)) target = buf;
    }

    if (!target.empty() && onBrowse) onBrowse(target);
    return S_OK;   // 告诉视图导航由浏览器接管（视图本身不自行跳转）
}
