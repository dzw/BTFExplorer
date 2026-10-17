#include "ShellFolderView.h"
#include "../Util/AppLog.h"
#include <shlwapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <cwchar>
#include <string>

namespace {

const wchar_t kHostClass[] = L"PEShellHostView";   // 每个窗格一个宿主窗口

// SetWindowSubclass 的标识：本类在同一窗口上只装这一种子类化（右键/改名按键拦截）
constexpr UINT_PTR kCtxSubclassId = 1;
// 内部列表可能是视图创建之后才建出来的、也会被 shell 重建：靠宿主窗口定时器盯着补装
constexpr UINT_PTR kCtxRetryTimer = 0x4352;

// 递归找视图内部的 SysListView32（shell 的右键菜单实际在这个窗口上处理）
HWND FindInnerListView(HWND parent)
{
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[64]{};
        if (GetClassNameW(c, cls, 64) > 0 && _wcsicmp(cls, L"SysListView32") == 0)
            return c;
        if (HWND deeper = FindInnerListView(c)) return deeper;
    }
    return nullptr;
}

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
    if (msg == WM_TIMER && wp == kCtxRetryTimer) {
        // 内部 SysListView32 由 shell 异步创建/重建（换目录、切视图模式）：
        // 子类化随旧窗口一起消失，句柄一变就补装，否则该窗口的 F2 收不到
        if (InnerListView() != ctxList_) InstallContextMenuSubclass();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShellFolderView::Layout()
{
    if (!host_ || !view_) return;
    RECT rc{};
    GetClientRect(host_, &rc);
    MoveWindow(view_, 0, 0, rc.right, rc.bottom, TRUE);
}

void ShellFolderView::UIActivateView(UINT uActivate)
{
    if (viewObj_) viewObj_->UIActivate(uActivate);
}

bool ShellFolderView::Navigate(const std::wstring& dir)
{
    if (!host_ || dir.empty()) return false;
    if (dir_ == dir && viewObj_) return true;   // 已在这个目录

    UniquePIDL pidl = shell::PIDLFromPath(dir);
    if (!pidl) return false;

    Destroy();   // 单视图生命周期内换目录：销毁再重建（切分页不再走这里）

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
    InstallViewSubclass();           // 拦下视图内文件夹激活，改为站内导航（见 ViewProc）
    // 拦下视图内右键/按键，改弹本程序菜单、站内改名（见 CtxProcStatic）；
    // 内部列表可能还没建出来、或之后被 shell 重建，靠宿主定时器盯着补装，否则 F2 收不到
    InstallContextMenuSubclass();
    if (host_) SetTimer(host_, kCtxRetryTimer, 250, nullptr);
    // 以“无焦点激活”挂上浏览器：视图才会向状态栏发条目数等消息
    viewObj_->UIActivate(SVUIA_ACTIVATE_NOFOCUS);
    return true;
}

void ShellFolderView::InstallViewSubclass()
{
    // 同一个视图窗口只装一次：pPriorView 复用窗口时 view_ 不变，重装会把本类的
    // ViewProcStatic 当作“原过程”存下来，形成自嵌套。换了新 HWND 才重装。
    if (!view_ || subclassedOn_ == view_) return;
    SetWindowLongPtrW(view_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    origViewProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(view_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ShellFolderView::ViewProcStatic)));
    subclassedOn_ = view_;
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

// --- 视图内右键：拦下 shell 自己的菜单，改弹本程序的菜单 ---
// （改名不再拦：DefView 自己的就地改名在本站点是通的——慢双击能触发就是证据，
//   之前按 F2 没反应只是因为这里把 F2 吞掉改成了对话框）

HWND ShellFolderView::InnerListView() const
{
    return view_ ? FindInnerListView(view_) : nullptr;
}

// 键盘消息转发给视图（浏览器职责）：DefView 把 F2 / Ctrl+C / Ctrl+V / Delete /
// Ctrl+A 等都做在 IShellView::TranslateAccelerator 里，靠站点转发；不转发这些键
// 在视图里就都没反应（鼠标路径不依赖它，所以一直正常）。
bool ShellFolderView::TranslateAccelerator(const MSG& msg)
{
    if (!viewObj_) return false;
    MSG m = msg;   // 接口要可写指针
    const HRESULT hr = viewObj_->TranslateAccelerator(&m);
    // 诊断：记录文件操作键（Delete / Ctrl+C/X/V/A / F5 / Backspace）的转发结果。
    // S_OK(0x00000000)=视图自认处理了；S_FALSE(0x00000001)=未处理（放行给窗口过程）。
    // 方向键等由内部列表原生处理，转发结果无关，不记录以免刷屏。
    if (msg.message == WM_KEYDOWN) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const UINT vk = static_cast<UINT>(msg.wParam);
        const bool interesting =
            (vk == VK_DELETE) || (ctrl && (vk == 'C' || vk == 'X' || vk == 'V' || vk == 'A'))
            || (vk == VK_F5) || (vk == VK_BACK);
        if (interesting) {
            wchar_t line[128];
            swprintf_s(line,
                L"SHELL TranslateAccel: msg=0x%04X vk=0x%02X ctrl=%d shift=%d -> 0x%08X",
                msg.message, vk, ctrl ? 1 : 0, shift ? 1 : 0, static_cast<unsigned>(hr));
            WriteAppLog(line);
        }
    }
    return hr == S_OK;
}

// 触发 shell 视图自己的就地改名：直接给内部列表发 LVM_EDITLABEL。
// 弹出来的就是 DefView 的 label edit 编辑框，慢双击最终也是这个，改名的提交
// 由 DefView 自己在 LVN_ENDLABELEDIT 里完成（与资源管理器一致）。
// 试过给列表投 VK_F2、也试过把 F2 整条放行给 shell，都不生效——DefView 的键盘
// 改名走 IShellView::TranslateAccelerator 的站点转发，本站点没接；鼠标路径
// （慢双击）不依赖它，所以能用。
bool ShellFolderView::StartInlineRename()
{
    HWND list = InnerListView();
    if (!list) {
        WriteAppLog(L"SHELL inline rename failed: inner list not found");
        return false;
    }
    const int item = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (item < 0) {
        WriteAppLog(L"SHELL inline rename failed: no selected item");
        return false;
    }
    HWND edit = ListView_EditLabel(list, item);
    // 视图可以在 LVN_BEGINLABELEDIT 里否决（那之后编辑框会被立刻销毁），这种情况
    // 也按失败处理，好让调用方退回对话框
    if (!edit || !IsWindow(edit)) {
        WriteAppLog(L"SHELL inline rename failed: edit box not created / rejected by view");
        return false;
    }
    // 选中主名（不含扩展名），与资源管理器一致
    wchar_t buf[MAX_PATH]{};
    GetWindowTextW(edit, buf, MAX_PATH);
    const std::wstring name(buf);
    const size_t dot = name.find_last_of(L'.');
    const LPARAM end = (dot != 0 && dot != std::wstring::npos)
                           ? static_cast<LPARAM>(dot) : static_cast<LPARAM>(-1);
    SendMessageW(edit, EM_SETSEL, 0, end);
    WriteAppLog((L"SHELL inline rename: edit box shown, item=" + std::to_wstring(item)).c_str());
    return true;
}

LRESULT CALLBACK ShellFolderView::CtxProcStatic(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                                UINT_PTR, DWORD_PTR ref)
{
    auto* self = reinterpret_cast<ShellFolderView*>(ref);
    if (!self) return DefSubclassProc(h, msg, wp, lp);
    // 真正的用户输入 -> 请求把本视图所在窗格设为激活窗格。只通知、不吞消息，
    // 视图自身的选中/打开行为完全不变（对等于自绘列表的 ActivatePaneDeferred）。
    if ((msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
         msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && self->onActivateRequest)
        self->onActivateRequest();
    if (msg == WM_CONTEXTMENU && self->TakeContextMenu(lp))
        return 0;   // 吞掉：不让 shell 弹它的原生菜单（没有本程序的自定义项）
    if (msg == WM_KEYDOWN && wp == VK_F2 && self->TakeRenameKey())
        return 0;   // 吞掉：DefView 自己的 F2 在本站点不生效，改用 LVM_EDITLABEL
    return DefSubclassProc(h, msg, wp, lp);
}

bool ShellFolderView::TakeRenameKey()
{
    if (!onRenameKey) return false;   // 没接线就交回给 shell
    onRenameKey();
    return true;
}

bool ShellFolderView::TakeContextMenu(LPARAM lp)
{
    if (!onContextMenu) return false;         // 没接线就放行原生菜单
    DWORD now = GetTickCount();
    // 同一次右键可能先落在内部列表、再转发到视图窗口：只认第一次
    if (static_cast<DWORD>(now - ctxTick_) < 200) return true;
    ctxTick_ = now;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (pt.x < 0 || pt.y < 0) GetCursorPos(&pt);   // 菜单键：lParam 是 (-1,-1)
    onContextMenu(pt);
    return true;
}

bool ShellFolderView::InstallContextMenuSubclass()
{
    if (!view_) return false;
    HWND list = InnerListView();
    HWND targets[2] = { list, view_ };
    for (HWND t : targets) {
        if (!t) continue;
        bool installed = false;
        for (HWND e : ctxSubs_) if (e == t) { installed = true; break; }
        if (installed) continue;
        if (SetWindowSubclass(t, &ShellFolderView::CtxProcStatic, kCtxSubclassId,
                              reinterpret_cast<DWORD_PTR>(this)))
            ctxSubs_.push_back(t);
    }
    // 内部列表被重建后，旧句柄上的子类化已随窗口消失：清掉记录，只留还活着的
    for (size_t i = 0; i < ctxSubs_.size();) {
        if (ctxSubs_[i] != list && ctxSubs_[i] != view_ && !IsWindow(ctxSubs_[i]))
            ctxSubs_.erase(ctxSubs_.begin() + i);
        else
            ++i;
    }
    ctxList_ = list;
    return list != nullptr;
}

void ShellFolderView::RemoveContextMenuSubclass()
{
    if (host_) KillTimer(host_, kCtxRetryTimer);
    for (HWND e : ctxSubs_) RemoveWindowSubclass(e, &ShellFolderView::CtxProcStatic, kCtxSubclassId);
    ctxSubs_.clear();
    ctxTick_ = 0;
    ctxList_ = nullptr;
}

// 光标下的条目路径：内部列表命中测试拿视图索引，再用 IFolderView2 取该条目的
// IShellItem。虚拟位置（无文件系统路径）返回空，调用方用选中项兜底。
std::wstring ShellFolderView::ItemPathAt(POINT screenPt, bool* outOnItem) const
{
    if (outOnItem) *outOnItem = false;
    HWND list = InnerListView();
    if (!list || !viewObj_) return {};
    POINT client = screenPt;
    ScreenToClient(list, &client);
    LVHITTESTINFO hit{};
    hit.pt = client;
    int idx = static_cast<int>(SendMessageW(list, LVM_HITTEST, 0,
                                           reinterpret_cast<LPARAM>(&hit)));
    if (idx < 0) return {};
    if (outOnItem) *outOnItem = true;

    ComPtr<IFolderView2> fv2;
    if (FAILED(viewObj_->QueryInterface(IID_PPV_ARGS(&fv2))) || !fv2) return {};
    // 先按 IUnknown 取回条目对象再判定它到底是哪种接口：GetItem 对不同 riid 的
    // 支持程度按 shell 版本而定，直接按猜的接口指针用会踩错虚表
    ComPtr<IUnknown> obj;
    if (FAILED(fv2->GetItem(idx, IID_PPV_ARGS(&obj))) || !obj) return {};
    ComPtr<IShellItem> item;
    if (FAILED(obj->QueryInterface(IID_PPV_ARGS(&item))) || !item) {
        ComPtr<IShellItemArray> arr;
        if (FAILED(obj->QueryInterface(IID_PPV_ARGS(&arr))) || !arr) return {};
        DWORD count = 0;
        arr->GetCount(&count);
        IShellItem* raw = nullptr;
        if (!count || FAILED(arr->GetItemAt(0, &raw)) || !raw) return {};
        item.Attach(raw);
    }
    PWSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        out = name;
        CoTaskMemFree(name);
    }
    return out;
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
    RemoveContextMenuSubclass();
    if (viewObj_) {
        viewObj_->DestroyViewWindow();
        viewObj_.Release();   // 视图析构时会 Release 本对象（IShellBrowser）
    }
    view_ = nullptr;
    subclassedOn_ = nullptr;    // 窗口随视图析构；下次新建需重装子类化
    origViewProc_ = nullptr;
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
        // 这里枚举到的是相对本文件夹的子 pidl，直接当绝对 pidl 用会被按当前目录
        // 解析出风马牛不相及的路径（必须先和视图目录拼成完整 pidl）
        PIDLIST_RELATIVE rel = nullptr;
        ULONG fetched = 0;
        if (items->Next(1, &rel, &fetched) != S_OK || fetched == 0 || !rel) break;
        UniquePIDL abs(ILCombine(static_cast<PCIDLIST_ABSOLUTE>(dirPidl_.get()), rel));
        CoTaskMemFree(rel);
        wchar_t buf[MAX_PATH * 2]{};
        if (abs && SHGetPathFromIDListW(static_cast<PCIDLIST_ABSOLUTE>(abs.get()), buf))
            out.push_back(buf);
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
