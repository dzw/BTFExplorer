#include "MainWindow.h"
#include "SettingsDialog.h"
#include "../Shell/ShellUtil.h"
#include "../Shell/ShellContextMenu.h"
#include "../Shell/ShellFileOperation.h"
#include "../Util/AppLog.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <algorithm>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

static constexpr int WM_APP_PAGELOADED = WM_APP + 1;
static constexpr UINT WM_APP_WIN_E = WM_APP + 2;   // Win+E 被拦截后激活本窗口
static constexpr int WM_APP_CLOSE_TAB = WM_APP + 3; // 中键点击分页 -> 延后到主窗口关闭
static constexpr int WM_APP_MOVE_TAB  = WM_APP + 4; // 拖拽分页 -> 延后到主窗口移动
static constexpr int WM_APP_ADD_TAB   = WM_APP + 6; // 双击空白新建分页 -> 延后到主窗口添加（+5 是托盘回调）
static constexpr int WM_APP_TREE_NEWTAB = WM_APP + 7; // 目录树中键 -> 延后到主窗口新开分页

// 当前焦点窗格顶部分页栏的整行底色（淡粉绿）：涂在分页头之间的空隙上，
// 分页头本身保持系统外观（见 PaneTabHandler 的 WM_PAINT）
static constexpr COLORREF kActiveTabStripColor = RGB(213, 234, 223);
                                                      // （wParam = new std::wstring*，主窗口负责释放）

static UINT GetTaskbarBroadcastMessage()
{
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

// 自定义通知值：Edit 没有 EN_RETURN 常量，回车通知用这个
static constexpr UINT EN_ADDR_RETURN = 0x1000;

// 分隔条：kSplitGap 是真正留白的视觉宽度，kSplitHit 是鼠标命中区宽度
// （命中区比留白宽，好抓；不改变布局）
static constexpr int kSplitGap = 3;
static constexpr int kSplitHit = 6;

// 缓慢双击进重命名：第一次单击选中后，第二次单击落在同一项、且间隔已超过
// 系统双击时间（说明没被识别成双击打开）但仍在该时间窗内 -> 视为“慢双击”重命名。
static constexpr DWORD kSlowRenameWindow = 1500; // ms

static LRESULT CALLBACK AddressProc(HWND h, UINT m, WPARAM wp, LPARAM lp); // 前向声明
static LRESULT CALLBACK PaneTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp); // 分页拖拽 tab 子类化
static LRESULT CALLBACK SideTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

// ListView 列
enum { COL_NAME = 0, COL_TYPE, COL_SIZE, COL_MTIME };

class FileDropTarget final : public IDropTarget {
public:
    FileDropTarget(MainWindow* owner, HWND list) : owner_(owner), list_(list) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL,
                                        DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        allowed_ = *effect;
        preferred_ = PreferredEffect(data);
        hasFiles_ = HasFileDrop(data);
        *effect = hasFiles_ ? ChooseEffect(keys) : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL, DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        *effect = hasFiles_ ? ChooseEffect(keys) : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        hasFiles_ = false;
        allowed_ = DROPEFFECT_NONE;
        preferred_ = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL,
                                   DWORD* effect) override
    {
        if (!effect) return E_POINTER;
        DWORD chosen = ChooseEffect(keys);
        if (!hasFiles_ || chosen == DROPEFFECT_NONE) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        FORMATETC format{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        HRESULT hr = data ? data->GetData(&format, &medium) : E_INVALIDARG;
        if (FAILED(hr)) {
            *effect = DROPEFFECT_NONE;
            return hr;
        }

        HDROP drop = static_cast<HDROP>(medium.hGlobal);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            UINT copied = DragQueryFileW(drop, i, path.data(), length + 1);
            if (copied > 0) {
                path.resize(copied);
                paths.push_back(std::move(path));
            }
        }
        ReleaseStgMedium(&medium);

        DWORD completed = owner_->HandleFileDrop(list_, paths, chosen);
        *effect = completed;
        DragLeave();
        return S_OK;
    }

private:
    static bool HasFileDrop(IDataObject* data)
    {
        FORMATETC format{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return data && SUCCEEDED(data->QueryGetData(&format));
    }

    static DWORD PreferredEffect(IDataObject* data)
    {
        static const CLIPFORMAT formatId =
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
        if (!data || !formatId) return DROPEFFECT_NONE;
        FORMATETC format{ formatId, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&format, &medium))) return DROPEFFECT_NONE;

        DWORD effect = DROPEFFECT_NONE;
        if (medium.hGlobal && GlobalSize(medium.hGlobal) >= sizeof(effect)) {
            const void* value = GlobalLock(medium.hGlobal);
            if (value) {
                effect = *static_cast<const DWORD*>(value);
                GlobalUnlock(medium.hGlobal);
            }
        }
        ReleaseStgMedium(&medium);
        return effect;
    }

    DWORD ChooseEffect(DWORD keys) const
    {
        DWORD available = allowed_;
        DWORD preferred = preferred_ & available;
        DWORD requested = (keys & MK_CONTROL) ? DROPEFFECT_COPY :
                          (keys & MK_SHIFT) ? DROPEFFECT_MOVE :
                          (preferred & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE :
                          (preferred & DROPEFFECT_COPY) ? DROPEFFECT_COPY :
                          (preferred & DROPEFFECT_LINK) ? DROPEFFECT_LINK :
                          (available & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE :
                          (available & DROPEFFECT_COPY) ? DROPEFFECT_COPY :
                          (available & DROPEFFECT_LINK) ? DROPEFFECT_LINK :
                          DROPEFFECT_NONE;
        return (requested & allowed_) ? requested : DROPEFFECT_NONE;
    }

    MainWindow* owner_;
    HWND list_;
    ULONG refs_ = 1;
    DWORD allowed_ = DROPEFFECT_NONE;
    DWORD preferred_ = DROPEFFECT_NONE;
    bool hasFiles_ = false;
};

// ---------------------------------------------------------------------------
// 创建
// ---------------------------------------------------------------------------
MainWindow* MainWindow::Create(HINSTANCE hInst)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcStatic;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 应用程序图标（资源 IDI_APP_ICON，提取自 Q-Dir，见 src/Resources/PagedExplorer.rc）
    HICON hAppIcon = reinterpret_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCE(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;   // 托盘/标题栏小图标：系统用大图标缩放派生
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PagedExplorerMain";
    RegisterClassExW(&wc);

    auto* self = new MainWindow();
    self->pageSize_ = 100;
    // 首个分页在 BuildChildren 里创建（那里才能拿到 tab 容器）

    RECT rcWork{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int w = rcWork.right - rcWork.left - 120, h = rcWork.bottom - rcWork.top - 120;

    self->hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"分页资源管理器 - Paged Explorer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, hInst, self);
    if (!self->hwnd_) { delete self; return nullptr; }

    self->BuildChildren();
    self->EnsureTrayIcon();     // 图标常驻：启动就挂上，窗口显示与否通知区都有
    self->RestoreSession();     // 恢复上次会话（窗格/分页/历史），无会话则默认 1 窗格
    self->SyncPagerSizeCombo(); // 会话里恢复的“每页项数”要反映到分页栏下拉框
    ShowWindow(self->hwnd_, self->startupShowCmd_);
    UpdateWindow(self->hwnd_);
    return self;
}

void MainWindow::EnsureTrayIcon()
{
    if (trayIcon_.Visible()) return;   // 已经常驻了
    bool added = trayIcon_.Add(hwnd_,
        reinterpret_cast<HICON>(GetClassLongPtrW(hwnd_, GCLP_HICONSM)));
    if (!added)
        WriteAppLog(L"TRAY_ICON ensure failed (Shell_NotifyIcon NIM_ADD failed)");
}

void MainWindow::ShowFromTray()
{
    // 图标常驻：这里不再 Remove，否则窗口一显示图标就从通知区消失了
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    if (activePane_ < panes_.size()) SetFocus(CurList());
}

void MainWindow::HideToTray()
{
    WriteAppLog(L"HIDE_TO_TRAY requested");
    EnsureTrayIcon();                  // 常驻图标；万一之前掉了（例如任务栏重启）这里补上
    if (!trayIcon_.Visible()) {
        WriteAppLog(L"HIDE_TO_TRAY failed because tray icon creation failed");
        MessageBoxW(hwnd_, L"无法创建系统托盘图标，应用仍保持打开。",
                    L"PagedExplorer", MB_OK | MB_ICONWARNING);
        return;
    }
    SaveSession();   // 收进托盘前先落盘，保证下次重启能恢复窗口尺寸/位置
    ShowWindow(hwnd_, SW_HIDE);
    WriteAppLog(L"HIDE_TO_TRAY window hidden");
}

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------
// 对话框本体在 SettingsDialog.cpp，这里只负责“取当前值 -> 交给对话框 -> 应用改动”。
void MainWindow::OpenSettings()
{
    SettingsData data;
    data.gridLines  = showGridLines_;
    data.pagination = paginationEnabled_;
    data.paneCount = (int)panes_.size();
    if (data.paneCount < 1) data.paneCount = 1;
    if (data.paneCount > 4) data.paneCount = 4;
    data.triLayout = triLayout_;
    if (!settings::GetAutoStart(data.autoStart)) return;
    const bool prevAutoStart = data.autoStart;

    // “确定/应用”时即时套用；“取消”不走到 apply。
    auto apply = [&](const SettingsData& d) {
        if (d.gridLines != showGridLines_) {
            showGridLines_ = d.gridLines;
            ApplyListStyles();
        }
        if (d.autoStart != prevAutoStart)
            settings::SetAutoStart(d.autoStart);
        if (d.pagination != paginationEnabled_) {
            paginationEnabled_ = d.pagination;
            CurTab().curPage = 0;
            Navigate(CurTab().dir, false);  // 用新的页大小重新加载
            Layout();                       // 显示/隐藏整条分页栏
        }
        if (d.triLayout != triLayout_)
            SetTriLayout(d.triLayout);
        if (d.paneCount != (int)panes_.size())
            SetPaneCount(d.paneCount);
    };
    settings::Show(hwnd_, uiFont_, data, apply);
    SetFocus(CurList());
}


void MainWindow::BuildChildren()
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    uiFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    auto mk = [&](DWORD style, DWORD ex, const wchar_t* cls, const wchar_t* text,
                  int id, HWND* out) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        if (uiFont_) SendMessageW(h, WM_SETFONT, (WPARAM)uiFont_, TRUE);
        if (out) *out = h;
        return h;
    };

    // 工具栏行：后退 前进 上级 刷新 + 地址栏
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"←", IDC_BACK, &btnBack_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"→", IDC_FORWARD, &btnFwd_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"↑", IDC_UP, &btnUp_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"刷新", IDC_REFRESH, &btnRefresh_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"菜单过滤词", IDC_MENU_FILTERS, &btnMenuFilters_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"设置", IDC_SETTINGS, &btnSettings_);
    mk(WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, WC_EDITW, L"", IDC_ADDRESS, &address_);
    // 子类化地址栏：原过程存 GWLP_USERDATA，回车跳转靠 AddressProc
    SetWindowLongPtrW(address_, GWLP_USERDATA,
                      GetWindowLongPtrW(address_, GWLP_WNDPROC));
    SetWindowLongPtrW(address_, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(&AddressProc));

    // 左侧 Tab 容器（目录树 / 收藏）
    CreateSidePanel();

    // 右侧窗格（文件列表）：首个窗格 + 一个分页
    panes_.emplace_back();
    CreatePane(panes_[0]);
    tabs_.emplace_back();
    tabs_[0].pane = 0;
    tabs_[0].pages->SetNotify([this]() { PostMessage(hwnd_, WM_APP_PAGELOADED, 0, 0); });
    panes_[0].tabs.push_back(0);
    // 首个窗格的首个分页也要有 tab 头：否则窗格没有标题条，“+”按钮会贴到窗格最左边
    TCITEMW ti0{};
    ti0.mask = TCIF_TEXT;
    ti0.pszText = const_cast<LPWSTR>(L"新建");
    SendMessageW(panes_[0].tab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ti0));

    // 分页栏
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"|◀", IDC_FIRST, &pagerFirst_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"◀", IDC_PREV, &pagerPrev_);
    mk(0, 0, WC_BUTTONW, L"", IDC_PAGER_LABEL, &pagerLabel_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"▶", IDC_NEXT, &pagerNext_);
    mk(WS_TABSTOP | BS_PUSHBUTTON, 0, WC_BUTTONW, L"▶|", IDC_LAST, &pagerLast_);
    mk(WS_TABSTOP | CBS_DROPDOWNLIST, 0, WC_COMBOBOXW, L"", IDC_PAGE_SIZE, &pagerSize_);
    for (int i = 0; i < kPageSizeCount; ++i) {
        wchar_t buf[32]; wsprintfW(buf, L"%d / 页", kPageSizes[i]);
        SendMessageW(pagerSize_, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(pagerSize_, CB_SETCURSEL, PageSizeToIndex((int)pageSize_), 0);

    // 状态栏
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(99)), hInst, nullptr);
    if (uiFont_) SendMessageW(status_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
}

void MainWindow::CreateSidePanel()
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    tab_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), hInst, nullptr);
    if (uiFont_) SendMessageW(tab_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    WNDPROC tabOld = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        tab_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SideTabProc)));
    SetWindowLongPtrW(tab_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tabOld));

    auto addTab = [&](const wchar_t* text, int id) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT | TCIF_PARAM;
        ti.pszText = const_cast<LPWSTR>(text);
        ti.lParam = id;
        SendMessageW(tab_, TCM_INSERTITEMW, id, reinterpret_cast<LPARAM>(&ti));
    };
    addTab(L"目录树", 0);
    addTab(L"收藏", 1);

    // 树和收藏列表都是 tab_ 的子窗口，显示由 SwitchSideTab 控制
    directoryTree_.Create(tab_, IDC_TREE, uiFont_);
    directoryTree_.PopulateDrives();
    // 目录树中键点击：在当前活动窗格新开分页打开该目录。
    // 不能在树的窗口过程里直接 AddRightTab（Layout 会重入树/页签控件导致
    // COMCTL32 崩溃，同双击新建分页的修复），堆上带路径延后到主窗口处理。
    directoryTree_.SetMiddleClickCallback([this](const std::wstring& path) {
        PostMessageW(hwnd_, WM_APP_TREE_NEWTAB,
                     reinterpret_cast<WPARAM>(new std::wstring(path)), 0);
    });
    btnTreeSync_ = CreateWindowExW(0, WC_BUTTONW, L"定",//定位
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, tab_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE_SYNC)), hInst, nullptr);
    if (uiFont_) SendMessageW(btnTreeSync_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    favList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER | LVS_SINGLESEL,
        0, 0, 0, 0, tab_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FAVLIST)), hInst, nullptr);
    if (uiFont_) SendMessageW(favList_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH, 0, 400, const_cast<LPWSTR>(L"目录") };
    ListView_InsertColumn(favList_, 0, &c);

    SwitchSideTab(0);
    LoadFavorites(); // 启动时读回 sort= 配置和收藏列表
}

void MainWindow::SwitchSideTab(int index)
{
    // 显示/隐藏 tab 页内容（树、收藏列表都是 tab_ 的子窗口）
    ShowWindow(directoryTree_.Handle(), index == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(btnTreeSync_, index == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(favList_, index == 1 ? SW_SHOW : SW_HIDE);
    SendMessageW(tab_, TCM_SETCURSEL, index, 0);
    if (index == 1) LoadFavorites();
    Layout();
}

// 系统图像列表只取一次，所有窗格共用（列表带 LVS_SHAREIMAGELISTS）
void MainWindow::BuildImageList(HWND list)
{
    if (!sysImgs_) {
        SHFILEINFOW fi{};
        sysImgs_ = reinterpret_cast<HIMAGELIST>(
            SHGetFileInfoW(L"C:\\", 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    }
    if (sysImgs_) {
        ListView_SetImageList(list, sysImgs_, LVSIL_SMALL);
        return;
    }
    if (!imgList_) {
        int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
        imgList_ = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 32, 64);
    }
    if (imgList_) ListView_SetImageList(list, imgList_, LVSIL_SMALL);
}

void MainWindow::SyncTreeToCurrentTab(bool showErrors)
{
    directoryTree_.SyncToPath(CurTab().dir, showErrors);
}

// 文件列表的扩展样式。网格线由设置界面的“显示网格线”开关控制，关掉时
// 只保留整行选中 + 双缓冲（双缓冲是拖分隔条不闪的前提，不能去掉）。
DWORD MainWindow::ListExStyle() const
{
    DWORD style = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
    if (showGridLines_) style |= LVS_EX_GRIDLINES;
    return style;
}

// 把当前开关状态套用到所有窗格列表（切换网格线时用）
void MainWindow::ApplyListStyles()
{
    for (auto& p : panes_)
        if (p.list) ListView_SetExtendedListViewStyle(p.list, ListExStyle());
}

// 每页项数变化或从会话恢复后，让分页栏的下拉框跟上（否则显示的还是旧档位）
void MainWindow::SyncPagerSizeCombo()
{
    if (!pagerSize_) return;
    SendMessageW(pagerSize_, CB_SETCURSEL, PageSizeToIndex((int)pageSize_), 0);
}

void MainWindow::CreatePane(Pane& p)
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    // 编号不直接用作窗格下标：删除窗格后下标会重排，编号保持唯一，避免 ID 撞车
    p.tag = AllocPaneTag();
    p.tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_TABS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RIGHTTAB + p.tag)), hInst, nullptr);
    if (uiFont_) SendMessageW(p.tab, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    // 子类化：支持分页拖拽到其它窗格
    SetWindowLongPtrW(p.tab, GWLP_USERDATA, GetWindowLongPtrW(p.tab, GWLP_WNDPROC));
    SetWindowLongPtrW(p.tab, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PaneTabProc));

    p.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SHAREIMAGELISTS,
        0, 0, 0, 0, p.tab, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_BASE + p.tag)), hInst, nullptr);
    if (uiFont_) SendMessageW(p.list, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ListView_SetExtendedListViewStyle(p.list, ListExStyle());
    BuildImageList(p.list);
    InsertColumns(p.list);

    p.listOld = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(p.list, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&MainWindow::ListViewProcStatic)));
    SetWindowLongPtrW(p.list, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    auto* dropTarget = new FileDropTarget(this, p.list);
    HRESULT dropResult = RegisterDragDrop(p.list, dropTarget);
    dropTarget->Release();
    if (FAILED(dropResult))
        WriteAppLog((L"RegisterDragDrop failed (" +
                     std::to_wstring(static_cast<unsigned long>(dropResult)) + L")").c_str());

    // 本窗格自己的“+”按钮（每个窗格一个，贴在最后一个分页头右侧）
    p.btnNewTab = CreateWindowExW(0, WC_BUTTONW, L"+",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 26, 22, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEWTAB_BASE + p.tag)), hInst, nullptr);
    if (uiFont_) SendMessageW(p.btnNewTab, WM_SETFONT, (WPARAM)uiFont_, TRUE);

    // 本窗格右上角的“▾”：外部工具下拉菜单（CMD / PowerShell / VSCode 等，见 tools.txt）
    p.btnTools = CreateWindowExW(0, WC_BUTTONW, L"▾",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 24, 20, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TOOLS_BASE + p.tag)), hInst, nullptr);
    if (uiFont_) SendMessageW(p.btnTools, WM_SETFONT, (WPARAM)uiFont_, TRUE);
}

// 分配当前未被占用的窗格编号（最多同时 4 个窗格，编号池 0~7 足够）
int MainWindow::AllocPaneTag()
{
    bool used[8] = { false };
    for (const auto& p : panes_)
        if (p.tag >= 0 && p.tag < 8) used[p.tag] = true;
    for (int i = 0; i < 8; ++i)
        if (!used[i]) return i;
    return (int)panes_.size();
}

// 销毁窗格（list 是 tab 的子窗口，随之销毁），并修正所有分页的 pane 下标
void MainWindow::RemovePane(size_t idx)
{
    if (idx >= panes_.size()) return;
    if (tabDragPane_ == (int)idx || (int)idx < tabDragPane_) {
        tabDragActive_ = false;    // 拖拽源/目标没了，别再引用旧下标
        tabDragPane_ = -1;
        tabDragIndex_ = -1;
    }
    if (panes_[idx].list) RevokeDragDrop(panes_[idx].list);
    DestroyWindow(panes_[idx].tab);
    if (panes_[idx].btnNewTab) DestroyWindow(panes_[idx].btnNewTab);
    if (panes_[idx].btnTools) DestroyWindow(panes_[idx].btnTools);
    panes_.erase(panes_.begin() + idx);
    for (auto& t : tabs_)
        if (t.pane > idx) --t.pane;
    if (panes_.empty()) activePane_ = 0;
    else if (activePane_ >= panes_.size()) activePane_ = panes_.size() - 1;
}

int MainWindow::PaneOfList(HWND h) const
{
    for (size_t i = 0; i < panes_.size(); ++i)
        if (panes_[i].list == h) return (int)i;
    return -1;
}

int MainWindow::PaneOfTab(HWND h) const
{
    for (size_t i = 0; i < panes_.size(); ++i)
        if (panes_[i].tab == h) return (int)i;
    return -1;
}

TabState& MainWindow::PaneActiveTab(size_t pi)
{
    if (pi >= panes_.size()) return CurTab();
    Pane& p = panes_[pi];
    if (p.tabs.empty()) return CurTab();
    if (p.active >= p.tabs.size()) p.active = p.tabs.size() - 1;
    return tabs_[p.tabs[p.active]];
}

std::wstring MainWindow::PaneItemPath(size_t pi, int item)
{
    if (pi >= panes_.size() || item < 0) return {};
    TabState& t = PaneActiveTab(pi);
    if (item >= (int)t.pageItems.size()) return {};
    return t.pageItems[item].path;
}

HWND MainWindow::CurList() const
{
    return panes_[activePane_].list;
}

// ---------------------------------------------------------------------------
// 右侧多窗格 / 多分页
// ---------------------------------------------------------------------------
void MainWindow::AddRightTab(bool navigateToDefault, size_t paneIdx)
{
    if (paneIdx == SIZE_MAX) paneIdx = activePane_;
    if (paneIdx >= panes_.size()) return;

    tabs_.emplace_back();
    size_t idx = tabs_.size() - 1;
    tabs_[idx].pane = paneIdx;
    tabs_[idx].pages->SetNotify([this]() {
        PostMessage(hwnd_, WM_APP_PAGELOADED, 0, 0);
    });
    panes_[paneIdx].tabs.push_back(idx);
    size_t inPane = panes_[paneIdx].tabs.size() - 1;
    panes_[paneIdx].active = inPane;

    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(L"新建");
    SendMessageW(panes_[paneIdx].tab, TCM_INSERTITEMW, inPane, reinterpret_cast<LPARAM>(&ti));

    activePane_ = paneIdx;
    activeTab_ = idx;
    Layout(); // 新 tab 加入后立即重排（否则控件停在初始位置造成重叠）
    if (navigateToDefault)
        Navigate(L"C:\\");
    else
        SelectRightTab(idx);

    // 虚拟列表在父级 tab 重排后不一定会重新绘制项目；显式刷新各窗格，
    // 否则其他窗格可能要等到获得焦点才显示已有内容。
    for (const Pane& pane : panes_)
        if (pane.list)
            RedrawWindow(pane.list, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void MainWindow::AddPane()
{
    size_t idx = panes_.size();
    panes_.emplace_back();
    CreatePane(panes_[idx]);
    Layout(); // 新窗格立即排布到正确位置（否则与现有窗格重叠）
    AddRightTab(true, idx); // 新窗格自带一个分页并激活
}

// Alt+1~4：窗格数量 1~4。减少时把多余窗格的分页并入最后一个保留窗格；
// 增加时逐个 AddPane。
void MainWindow::SetPaneCount(int n)
{
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    // 减少：把末尾窗格的分页逐个移进第 n-1 个窗格（MoveTabToPane 会自动关闭空窗格）；
    // 目标窗格已有相同目录的分页时，直接丢弃这个重复分页，不再搬过去
    const size_t target = (size_t)n - 1;
    while ((int)panes_.size() > n) {
        Pane& last = panes_.back();
        if (last.tabs.empty()) {
            RemovePane(panes_.size() - 1);
            continue;
        }
        size_t tabIndex = last.tabs[0];
        if (PaneHasDir(target, tabs_[tabIndex].dir)) {
            bool emptied = false;
            RemoveTab(tabIndex, emptied);
            if (emptied && panes_.size() > 1)
                RemovePane(panes_.size() - 1);
            continue;
        }
        MoveTabToPane(tabIndex, target);
    }
    // 增加
    while ((int)panes_.size() < n)
        AddPane();

    layoutCount_ = (int)panes_.size();
    if (activePane_ >= panes_.size()) activePane_ = panes_.size() - 1;
    Pane& p = panes_[activePane_];
    if (!p.tabs.empty()) {
        if (p.active >= p.tabs.size()) p.active = p.tabs.size() - 1;
        SelectRightTab(p.tabs[p.active]);
    }
    UpdateRightTabLabels();
    Layout();
    SaveFavorites(); // 记住窗格数量，下次启动沿用
}

// 3 窗格形态：0=品字形(1上2下) 1=倒品字形(2上1下)
// 无论当前几个窗格都先记住选择（并持久化），三窗格时立刻生效
void MainWindow::SetTriLayout(int t)
{
    int v = t ? 1 : 0;
    bool changed = (triLayout_ != v);
    triLayout_ = v;
    if (panes_.size() == 3) Layout();
    if (changed) SaveFavorites();
}

// 启动时恢复上次布局：没有记录就用默认的倒品字形
void MainWindow::ApplySavedLayout()
{
    if (startupLayoutApplied_) return;
    startupLayoutApplied_ = true;
    if (savedPaneCount_ > 1 && savedPaneCount_ != (int)panes_.size())
        SetPaneCount(savedPaneCount_);
}

// 销毁全部窗格与分页（窗格的 list 是 tab 的子窗口，随 DestroyWindow 一起没）
void MainWindow::ResetAllPanes()
{
    while (!panes_.empty())
        RemovePane(panes_.size() - 1);
    tabs_.clear();
    activePane_ = 0;
    activeTab_ = 0;
}

// 退出时把当前所有窗格/分页/各自的历史写盘（UTF-8，与 favorites.txt 同目录）
void MainWindow::SaveSession()
{
    std::wstring path = SessionFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    fwrite("\xEF\xBB\xBF", 1, 3, f);   // UTF-8 BOM

    auto putLine = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return;
        std::string buf(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                            &buf[0], n, nullptr, nullptr);
        buf += '\n';
        fwrite(buf.data(), 1, buf.size(), f);
    };

    putLine(L"sort=" + std::to_wstring(sortCol_) + L"," + std::to_wstring(sortAsc_ ? 1 : 0));
    putLine(L"panes=" + std::to_wstring(panes_.size()));
    putLine(L"tri=" + std::to_wstring(triLayout_));
    putLine(L"sel=" + std::to_wstring(activeTab_));
    putLine(L"sideWidth=" + std::to_wstring(sideWidth_));
    // 设置界面里的开关也要记住，重启后继续生效
    putLine(L"grid=" + std::to_wstring(showGridLines_ ? 1 : 0));
    putLine(L"pagination=" + std::to_wstring(paginationEnabled_ ? 1 : 0));
    putLine(L"pageSize=" + std::to_wstring(pageSize_));
    // 记录窗口位置/尺寸。
    // 非最大化/最小化时改用 GetWindowRect 取“真实屏幕矩形”，这样能正确捕获
    // Aero Snap（Win+←/→）后的半屏位置/尺寸；若用 GetWindowPlacement 的
    // rcNormalPosition，Snap 窗口会返回未 Snap 前的旧矩形，导致重启后不恢复。
    RECT r{};
    int maxFlag = 0;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    bool gp = (GetWindowPlacement(hwnd_, &placement) != 0);
    bool maximized = gp && (placement.showCmd == SW_SHOWMAXIMIZED);
    bool minimized = gp && (placement.showCmd == SW_SHOWMINIMIZED);
    if (maximized) {
        r = placement.rcNormalPosition;   // 最大化：存“还原”矩形，重启后恢复为最大化
        maxFlag = 1;
    } else if (minimized) {
        r = placement.rcNormalPosition;   // 最小化：存正常矩形，避免存图标位置
    } else if (!GetWindowRect(hwnd_, &r) && gp) {
        r = placement.rcNormalPosition;
    }
    if (r.right > r.left && r.bottom > r.top) {
        std::wstring wline = L"window=" + std::to_wstring(r.left) + L"," +
                std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," +
                std::to_wstring(r.bottom) + L"," + std::to_wstring(maxFlag);
        putLine(wline);
        WriteAppLog((L"SAVE session window rect (src=" +
            std::wstring(maximized ? L"max" : (minimized ? L"min" : L"screen")) +
            L"): " + wline).c_str());
    } else {
        WriteAppLog(L"SAVE window rect invalid; no window= line written");
    }
    std::wstring widths;
    for (size_t i = 0; i < panes_.size(); ++i) {
        if (i) widths += L",";
        widths += std::to_wstring(panes_[i].width);
    }
    putLine(L"widths=" + widths);

    for (size_t i = 0; i < tabs_.size(); ++i) {
        const TabState& t = tabs_[i];
        putLine(L"[tab]");
        putLine(L"pane=" + std::to_wstring(t.pane));
        putLine(L"locked=" + std::to_wstring(t.locked ? 1 : 0));
        putLine(L"hist=" + std::to_wstring(t.histPos));
        std::wstring hist = t.history.empty() ? t.dir : t.history[0];
        for (size_t k = 1; k < t.history.size(); ++k) {
            hist += L"|";
            hist += t.history[k];
        }
        putLine(L"history=" + hist);
    }
    fclose(f);
}

// 启动恢复上次会话。命中且至少有一个分页则重建；否则沿用默认（1 窗格 + 默认目录）
bool MainWindow::RestoreSession()
{
    // 恢复期间抑制 SaveSession（SetWindowPlacement 会触发 WM_WINDOWPOSCHANGED），
    // 避免刚恢复完又立刻把“恢复后的矩形”写回，造成无意义写盘/潜在递归。
    struct RestoreGuard { bool* p; RestoreGuard(bool* p_) : p(p_) { *p_ = true; } ~RestoreGuard() { *p = false; } };
    RestoreGuard guard(&restoreInProgress_);
    // ---- 读盘 ----
    std::wstring path = SessionFilePath();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    WriteAppLog((L"RESTORE session file path=" + path +
        (f ? L" (opened)" : L" (NOT opened)")).c_str());
    struct TabRec { int pane = 0; int locked = 0; int histPos = 0; std::vector<std::wstring> history; };
    int paneCount = 1, tri = 1, sel = 0, sortCol = 0, sortAsc = 1;
    int savedSideWidth = sideWidth_, windowMaximized = 0;
    RECT savedWindowRect{};
    bool hasSavedWindowRect = false;
    std::vector<int> widths;
    std::vector<TabRec> recs;
    TabRec cur; bool inTab = false, ok = false;

    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        std::string data; data.resize(sz > 0 ? sz : 0);
        if (sz > 0) fread(&data[0], 1, sz, f);
        fclose(f);
        if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
            (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
            data.erase(0, 3);
        size_t pos = 0;
        while (pos < data.size()) {
            size_t eol = data.find('\n', pos);
            std::string line = (eol == std::string::npos) ? data.substr(pos)
                                                          : data.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = (eol == std::string::npos) ? data.size() : eol + 1;
            if (line.empty()) continue;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
            if (wlen <= 0) continue;
            std::wstring w(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), w.data(), wlen);
            if (w == L"[tab]") { if (inTab) recs.push_back(cur); cur = TabRec(); inTab = true; continue; }
            if (w.rfind(L"sort=", 0) == 0) { swscanf_s(w.c_str() + 5, L"%d,%d", &sortCol, &sortAsc); continue; }
            if (w.rfind(L"panes=", 0) == 0) { swscanf_s(w.c_str() + 6, L"%d", &paneCount); continue; }
            if (w.rfind(L"tri=", 0) == 0) { swscanf_s(w.c_str() + 4, L"%d", &tri); continue; }
            if (w.rfind(L"sel=", 0) == 0) { swscanf_s(w.c_str() + 4, L"%d", &sel); continue; }
            if (w.rfind(L"sideWidth=", 0) == 0) {
                swscanf_s(w.c_str() + 10, L"%d", &savedSideWidth);
                continue;
            }
            if (w.rfind(L"window=", 0) == 0) {
                int left = 0, top = 0, right = 0, bottom = 0;
                if (swscanf_s(w.c_str() + 7, L"%d,%d,%d,%d,%d",
                              &left, &top, &right, &bottom, &windowMaximized) == 5 &&
                    right > left && bottom > top) {
                    savedWindowRect = { left, top, right, bottom };
                    hasSavedWindowRect = true;
                    WriteAppLog((L"RESTORE parsed window rect: left=" + std::to_wstring(left) +
                        L" top=" + std::to_wstring(top) + L" right=" + std::to_wstring(right) +
                        L" bottom=" + std::to_wstring(bottom) +
                        L" max=" + std::to_wstring(windowMaximized)).c_str());
                } else {
                    WriteAppLog(L"RESTORE window= line present but parse failed or invalid (right<=left or bottom<=top)");
                }
                continue;
            }
            if (w.rfind(L"widths=", 0) == 0) {
                std::wstring v = w.substr(7);
                for (size_t p = 0; p <= v.size(); ) {
                    size_t c = v.find(L',', p);
                    std::wstring tok = (c == std::wstring::npos) ? v.substr(p) : v.substr(p, c - p);
                    if (!tok.empty()) widths.push_back(_wtoi(tok.c_str()));
                    if (c == std::wstring::npos) break;
                    p = c + 1;
                }
                continue;
            }
            if (w.rfind(L"grid=", 0) == 0) {
                showGridLines_ = (_wtoi(w.c_str() + 5) != 0);
                continue;
            }
            if (w.rfind(L"pagination=", 0) == 0) {
                paginationEnabled_ = (_wtoi(w.c_str() + 11) != 0);
                continue;
            }
            if (w.rfind(L"pageSize=", 0) == 0) {
                int v = _wtoi(w.c_str() + 9);
                // 只接受下拉框里真实存在的档位，避免手改文件后把分页搞坏
                for (int i = 0; i < kPageSizeCount; ++i)
                    if (v == kPageSizes[i]) { pageSize_ = (size_t)v; break; }
                continue;
            }
            if (inTab) {
                if (w.rfind(L"pane=", 0) == 0) cur.pane = _wtoi(w.c_str() + 5);
                else if (w.rfind(L"locked=", 0) == 0) cur.locked = _wtoi(w.c_str() + 7);
                else if (w.rfind(L"hist=", 0) == 0) cur.histPos = _wtoi(w.c_str() + 5);
                else if (w.rfind(L"history=", 0) == 0) {
                    std::wstring v = w.substr(8);
                    cur.history.clear();
                    if (v.empty()) cur.history.push_back(L"");
                    for (size_t p = 0; p <= v.size(); ) {
                        size_t c = v.find(L'|', p);
                        cur.history.push_back((c == std::wstring::npos) ? v.substr(p) : v.substr(p, c - p));
                        if (c == std::wstring::npos) break;
                        p = c + 1;
                    }
                }
            }
        }
        if (inTab) recs.push_back(cur);
        ok = !recs.empty();
    }
    if (!f) WriteAppLog(L"RESTORE session file not opened (no saved session)");

    if (hasSavedWindowRect) {
        // 诊断：恢复前的当前窗口矩形（Create 时设定的默认尺寸）
        {
            WINDOWPLACEMENT curWp{}; curWp.length = sizeof(curWp);
            if (GetWindowPlacement(hwnd_, &curWp)) {
                const RECT& r = curWp.rcNormalPosition;
                WriteAppLog((L"RESTORE pre-restore window rect: left=" + std::to_wstring(r.left) +
                    L" top=" + std::to_wstring(r.top) + L" right=" + std::to_wstring(r.right) +
                    L" bottom=" + std::to_wstring(r.bottom)).c_str());
            }
        }
        // 防御：保存的矩形若不在任何显示器上（显示器断开 / 分辨率或 DPI 变化），
        // 夹到主显示器工作区，避免窗口落到屏幕外而“看起来没恢复尺寸”
        if (!MonitorFromRect(&savedWindowRect, MONITOR_DEFAULTTONULL)) {
            WriteAppLog(L"RESTORE saved window rect off-screen; clamping to primary work area");
            RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int wdt = savedWindowRect.right - savedWindowRect.left;
            int hgt = savedWindowRect.bottom - savedWindowRect.top;
            if (wdt > wa.right - wa.left) wdt = wa.right - wa.left;
            if (hgt > wa.bottom - wa.top) hgt = wa.bottom - wa.top;
            savedWindowRect = { wa.left, wa.top, wa.left + wdt, wa.top + hgt };
        }
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        placement.showCmd = windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        placement.rcNormalPosition = savedWindowRect;
        BOOL placed = SetWindowPlacement(hwnd_, &placement);
        WriteAppLog((L"RESTORE SetWindowPlacement ret=" + std::to_wstring(placed ? 1 : 0) +
            L" err=" + std::to_wstring(placed ? 0 : GetLastError())).c_str());
        if (!placed)
            SetWindowPos(hwnd_, nullptr, savedWindowRect.left, savedWindowRect.top,
                         savedWindowRect.right - savedWindowRect.left,
                         savedWindowRect.bottom - savedWindowRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        // 验证实际生效的矩形
        WINDOWPLACEMENT after{}; after.length = sizeof(after);
        if (GetWindowPlacement(hwnd_, &after)) {
            const RECT& r = after.rcNormalPosition;
            WriteAppLog((L"RESTORE applied window rect: left=" + std::to_wstring(r.left) +
                L" top=" + std::to_wstring(r.top) + L" right=" + std::to_wstring(r.right) +
                L" bottom=" + std::to_wstring(r.bottom) +
                L" showCmd=" + std::to_wstring(after.showCmd)).c_str());
        }
        startupShowCmd_ = windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOW;
    } else {
        WriteAppLog(L"RESTORE no saved window rect; using default window size");
    }
    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);
    int maxSideWidth = clientRect.right - 260;
    if (maxSideWidth < 150) maxSideWidth = 150;
    sideWidth_ = (savedSideWidth < 150) ? 150 :
                 (savedSideWidth > maxSideWidth ? maxSideWidth : savedSideWidth);

    if (!ok) {
        WriteAppLog(L"RESTORE no tabs in session; using default layout (no window restore)");
        // 默认：沿用旧逻辑（favorites.txt 的 panes/tri + 默认目录）
        ApplySavedLayout();
        Navigate(L"C:\\Users\\Public", false);
        return false;
    }

    // ---- 重建 ----
    if (paneCount < 1) paneCount = 1;
    if ((size_t)paneCount > recs.size()) paneCount = (int)recs.size(); // 每个窗格至少 1 分页
    ResetAllPanes();
    for (int i = 0; i < paneCount; ++i) {
        panes_.emplace_back();
        CreatePane(panes_.back());
        if (i < (int)widths.size() && widths[i] > 0) panes_.back().width = widths[i];
    }
    for (const TabRec& r : recs) {
        size_t pi = (size_t)r.pane;
        if (pi >= panes_.size()) pi = panes_.size() - 1;
        tabs_.emplace_back();
        size_t idx = tabs_.size() - 1;
        TabState& t = tabs_[idx];
        t.pane = pi;
        t.locked = (r.locked != 0);
        t.history = r.history;
        if (t.history.empty()) t.history.push_back(L"");
        t.histPos = r.histPos;
        if (t.histPos < 0) t.histPos = 0;
        if ((size_t)t.histPos >= t.history.size()) t.histPos = (int)t.history.size() - 1;
        t.dir = t.history[t.histPos];
        t.pages->SetNotify([this]() { PostMessage(hwnd_, WM_APP_PAGELOADED, 0, 0); });
        t.pages->OpenDirectory(t.dir, EffectivePageSize());  // 预置加载器：之后切到该分页时 RequestPage 能命中
        panes_[pi].tabs.push_back(idx);
        std::wstring name = t.dir;
        size_t s = name.find_last_of(L'\\');
        if (s != std::wstring::npos && s + 1 < name.size()) name = name.substr(s + 1);
        else if (name.size() >= 2 && name[1] == L':') name = name.substr(0, 2);
        if (name.empty()) name = L"新建";
        if (t.locked) name = L"[锁] " + name;
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(name.c_str());
        SendMessageW(panes_[pi].tab, TCM_INSERTITEMW, panes_[pi].tabs.size() - 1, reinterpret_cast<LPARAM>(&ti));
    }

    // 去掉没有分页的空窗格（损坏的会话文件可能缺页），保证窗格连续
    for (int i = (int)panes_.size() - 1; i >= 0; --i)
        if (panes_[i].tabs.empty()) RemovePane(i);

    // 排序 / 布局 / 品字形态
    if (sortCol >= 0 && sortCol <= 3) { sortCol_ = sortCol; sortAsc_ = (sortAsc != 0); }
    triLayout_ = (tri != 0) ? 1 : 0;
    savedPaneCount_ = (int)panes_.size();

    // 激活上次的分页
    size_t act = (sel >= 0 && (size_t)sel < tabs_.size()) ? (size_t)sel : tabs_.size() - 1;
    activeTab_ = act;
    activePane_ = tabs_[act].pane;
    SelectRightTab(act);
    UpdateRightTabLabels();
    Layout();
    return true;
}

void MainWindow::SelectRightTab(size_t index)
{
    if (index >= tabs_.size()) return;
    TabState& t = tabs_[index];
    size_t prevPane = activePane_;   // 旧焦点窗格：重画分页栏时只需新旧两个
    activeTab_ = index;
    activePane_ = t.pane;
    Pane& p = panes_[activePane_];
    for (size_t k = 0; k < p.tabs.size(); ++k)
        if (p.tabs[k] == index) { p.active = k; break; }
    SendMessageW(p.tab, TCM_SETCURSEL, p.active, 0);
    SetWindowTextW(address_, t.dir.c_str());
    InvalidateTabStrips(prevPane);
    RefreshList();
    SyncTreeToCurrentTab(false);
}

void MainWindow::SelectPane(size_t index)
{
    if (index >= panes_.size()) return;
    Pane& p = panes_[index];
    if (p.tabs.empty()) return;
    size_t prevPane = activePane_;   // 旧焦点窗格：重画分页栏时只需新旧两个
    activePane_ = index;
    activeTab_ = p.tabs[p.active];
    SetWindowTextW(address_, CurTab().dir.c_str());
    InvalidateTabStrips(prevPane);
    RefreshList();
    SyncTreeToCurrentTab(false);
}

// 焦点窗格变化后重画受影响窗格的分页栏（焦点窗格整行淡粉绿）。
// 只失效“旧焦点 + 新焦点”两个窗格、且只失效分页栏那一行：
//   - 其他窗格的分页栏外观与焦点无关，整块失效只会带来无谓重绘，
//     表现为切换分页/窗格时其他窗格闪一下；
//   - tab 控件客户区还包括列表四周的边距，整块失效会让整个窗格
//     的框线跟着重绘（又是闪）。
void MainWindow::InvalidateTabStrips(size_t prevPane)
{
    if (prevPane == activePane_) return;   // 同窗格内换分页头：分页栏外观不变
    for (size_t i = 0; i < panes_.size(); ++i) {
        if (i != activePane_ && i != prevPane) continue;
        HWND tab = panes_[i].tab;
        if (!tab) continue;
        int cnt = (int)SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0);
        if (cnt <= 0) continue;
        RECT ti{};
        if (!SendMessageW(tab, TCM_GETITEMRECT, (WPARAM)(cnt - 1), reinterpret_cast<LPARAM>(&ti)) &&
            !SendMessageW(tab, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&ti)))
            continue;
        RECT rc{};
        if (!GetClientRect(tab, &rc)) continue;
        int rowH = ti.bottom + 3;          // 与 PaneTabHandler WM_PAINT 的行高算法一致
        if (rowH > rc.bottom) rowH = rc.bottom;
        RECT row = { 0, 0, rc.right, rowH };
        InvalidateRect(tab, &row, FALSE);
    }
}

void MainWindow::MoveTabToPane(size_t tabIndex, size_t paneIdx)
{
    if (tabIndex >= tabs_.size() || paneIdx >= panes_.size()) return;
    size_t oldPane = tabs_[tabIndex].pane;
    if (oldPane == paneIdx) return;

    Pane& from = panes_[oldPane];
    Pane& to = panes_[paneIdx];

    // 从源窗格 tab 控件删除
    for (size_t k = 0; k < from.tabs.size(); ++k)
        if (from.tabs[k] == tabIndex) {
            SendMessageW(from.tab, TCM_DELETEITEM, k, 0);
            from.tabs.erase(from.tabs.begin() + k);
            break;
        }
    if (from.active >= from.tabs.size() && !from.tabs.empty())
        from.active = from.tabs.size() - 1;

    // 插入目标窗格
    tabs_[tabIndex].pane = paneIdx;
    to.tabs.push_back(tabIndex);
    size_t inPane = to.tabs.size() - 1;
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    std::wstring name = tabs_[tabIndex].dir;
    size_t s = name.find_last_of(L'\\');
    if (s != std::wstring::npos && s + 1 < name.size()) name = name.substr(s + 1);
    if (name.empty()) name = L"新建";
    ti.pszText = const_cast<LPWSTR>(name.c_str());
    SendMessageW(to.tab, TCM_INSERTITEMW, inPane, reinterpret_cast<LPARAM>(&ti));
    to.active = inPane;

    // 源窗格空了：关掉它
    if (panes_[oldPane].tabs.empty())
        RemovePane(oldPane);
    SelectRightTab(tabIndex);
    Layout();
}

void MainWindow::UpdateRightTabLabels()
{
    for (size_t pi = 0; pi < panes_.size(); ++pi) {
        Pane& p = panes_[pi];
        for (size_t k = 0; k < p.tabs.size(); ++k) {
            std::wstring name = tabs_[p.tabs[k]].dir;
            size_t s = name.find_last_of(L'\\');
            if (s != std::wstring::npos && s + 1 < name.size()) name = name.substr(s + 1);
            else if (name.size() >= 2 && name[1] == L':') name = name.substr(0, 2);
            if (name.empty()) name = L"新建";
            if (tabs_[p.tabs[k]].locked) name = L"[锁] " + name;
            // 文本没变就不重设：TCM_SETITEMW 会让分页头失效重绘，
            // 一次导航会把所有窗格的所有分页头全刷一遍（闪）
            wchar_t cur[512]{};
            TCITEMW curItem{};
            curItem.mask = TCIF_TEXT;
            curItem.pszText = cur;
            curItem.cchTextMax = 512;
            if (SendMessageW(p.tab, TCM_GETITEMW, k, reinterpret_cast<LPARAM>(&curItem)) &&
                wcscmp(cur, name.c_str()) == 0)
                continue;
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(name.c_str());
            SendMessageW(p.tab, TCM_SETITEMW, k, reinterpret_cast<LPARAM>(&ti));
        }
    }
    // 标题宽度变化会移动最后一个 tab 头右缘：立即同步各窗格“+”按钮，避免重叠/错位
    UpdateNewTabButtons();
}

// 分页在本窗格里的序号（找不到返回 SIZE_MAX）
size_t MainWindow::PaneTabPos(size_t paneIdx, size_t tabIndex) const
{
    if (paneIdx >= panes_.size()) return SIZE_MAX;
    const Pane& p = panes_[paneIdx];
    for (size_t k = 0; k < p.tabs.size(); ++k)
        if (p.tabs[k] == tabIndex) return k;
    return SIZE_MAX;
}

// 分页标题右键菜单：关闭 / 关闭其他 / 关闭右边 / 锁定
void MainWindow::TabContextMenu(HWND h, int idx, POINT screenPt)
{
    int pi = PaneOfTab(h);
    if (pi < 0 || idx < 0 || (size_t)idx >= panes_[pi].tabs.size()) return;
    menuTab_ = panes_[pi].tabs[idx];

    bool lastOne = (tabs_.size() <= 1);
    bool locked = tabs_[menuTab_].locked;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (locked ? MF_CHECKED : 0), IDC_TM_LOCK, L"锁定(&L)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // 锁定 / 只剩这一个时不允许关闭
    UINT dis = (locked || lastOne) ? (MF_DISABLED | MF_GRAYED) : 0;
    AppendMenuW(m, MF_STRING | dis, IDC_TM_CLOSE,  L"关闭(&C)");
    AppendMenuW(m, MF_STRING,      IDC_TM_OTHERS, L"关闭其他(&O)");
    AppendMenuW(m, MF_STRING,      IDC_TM_RIGHT,  L"关闭右边(&R)");
    // 不要加 TPM_RETURNCMD：那会让菜单直接返回命令 ID 而不发送 WM_COMMAND，
    // 下面的 IDC_TM_* 分支就永远收不到，点了没反应。
    TrackPopupMenuEx(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                     screenPt.x, screenPt.y, hwnd_, nullptr);
    DestroyMenu(m);
}

// 关闭同一窗格里除指定分页之外的分页（锁定的保留）
void MainWindow::CloseOtherTabs(size_t keepTabIndex)
{
    if (keepTabIndex >= tabs_.size()) return;
    size_t paneIdx = tabs_[keepTabIndex].pane;
    if (paneIdx >= panes_.size()) return;

    bool any = false;
    for (;;) {
        size_t victim = SIZE_MAX;
        for (size_t k : panes_[paneIdx].tabs)
            if (k != keepTabIndex && !tabs_[k].locked) victim = k;
        if (victim == SIZE_MAX) break;
        bool emptied = false;
        RemoveTab(victim, emptied);         // 从后往前删，下标不会串
        any = true;
        if (emptied) break;                 // 理论不会发生（keep 还在）
    }
    if (any) {
        UpdateRightTabLabels();
        Layout();
    }
}

// 关闭同一窗格中指定分页右侧的所有分页（锁定的保留）
void MainWindow::CloseRightTabs(size_t keepTabIndex)
{
    if (keepTabIndex >= tabs_.size()) return;
    size_t paneIdx = tabs_[keepTabIndex].pane;
    if (paneIdx >= panes_.size()) return;
    size_t keepPos = PaneTabPos(paneIdx, keepTabIndex);
    if (keepPos == SIZE_MAX) return;

    bool any = false;
    for (;;) {
        size_t victim = SIZE_MAX;
        for (size_t k = keepPos + 1; k < panes_[paneIdx].tabs.size(); ++k)
            if (!tabs_[panes_[paneIdx].tabs[k]].locked)
                victim = panes_[paneIdx].tabs[k];   // 取最右一个可关的
        if (victim == SIZE_MAX) break;
        bool emptied = false;
        RemoveTab(victim, emptied);
        any = true;
        keepPos = PaneTabPos(paneIdx, keepTabIndex);
        if (keepPos == SIZE_MAX) break;
    }
    if (any) {
        UpdateRightTabLabels();
        Layout();
    }
}

void MainWindow::ToggleTabLock(size_t tabIndex)
{
    if (tabIndex >= tabs_.size()) return;
    tabs_[tabIndex].locked = !tabs_[tabIndex].locked;
    UpdateRightTabLabels();   // 标题前加 [锁]
    SaveSession();            // 立刻落盘：锁定是用户明确要的语义，别等退出时才存
}

// 目标窗格里是否已经有同一目录的分页（忽略大小写与结尾多余的 \）
bool MainWindow::PaneHasDir(size_t paneIdx, const std::wstring& dir) const
{
    if (paneIdx >= panes_.size() || dir.empty()) return false;
    auto trim = [](const std::wstring& s) {
        size_t n = s.size();
        while (n > 0 && s[n - 1] == L'\\') --n;  // "C:\\Users\\" -> "C:\\Users"（根盘符 "C:\" -> "C:"）
        return std::make_pair(s.c_str(), n);
    };
    auto [pd, pn] = trim(dir);
    for (size_t tabIdx : panes_[paneIdx].tabs) {
        const std::wstring& d = tabs_[tabIdx].dir;
        if (d.empty()) continue;
        auto [qd, qn] = trim(d);
        if (pn == qn && _wcsnicmp(pd, qd, pn) == 0) return true;
    }
    return false;
}

// 删除一个分页：从所属窗格的 tab 控件删项、停止加载、从 tabs_ 移除并修正所有下标。
// paneEmptied 返回所属窗格是否因此变空（是否 RemovePane 由调用方决定）
void MainWindow::RemoveTab(size_t index, bool& paneEmptied)
{
    paneEmptied = false;
    if (index >= tabs_.size()) return;
    size_t paneIdx = tabs_[index].pane;
    if (paneIdx >= panes_.size()) return;

    Pane& p = panes_[paneIdx];
    for (size_t k = 0; k < p.tabs.size(); ++k)
        if (p.tabs[k] == index) {
            SendMessageW(p.tab, TCM_DELETEITEM, k, 0);
            break;
        }

    tabs_[index].pages->Shutdown();
    tabs_.erase(tabs_.begin() + index);
    // 窗格里记录的 tab 下标整体前移（pane 下标不受影响）
    for (auto& pp : panes_) {
        for (size_t k = 0; k < pp.tabs.size();) {
            if (pp.tabs[k] == index) pp.tabs.erase(pp.tabs.begin() + k);
            else { if (pp.tabs[k] > index) --pp.tabs[k]; ++k; }
        }
        if (pp.active >= pp.tabs.size() && !pp.tabs.empty()) pp.active = pp.tabs.size() - 1;
    }
    if (activeTab_ > index) --activeTab_;
    else if (activeTab_ >= tabs_.size() && !tabs_.empty()) activeTab_ = tabs_.size() - 1;
    paneEmptied = p.tabs.empty();
}

void MainWindow::CloseRightTab(size_t index)
{
    if (tabs_.size() <= 1 || index >= tabs_.size()) return; // 至少保留一个
    if (tabs_[index].locked) return;                        // 锁定的不关
    size_t paneIdx = tabs_[index].pane;
    if (paneIdx >= panes_.size()) return;
    bool emptied = false;
    RemoveTab(index, emptied);
    if (emptied && panes_.size() > 1) {
        RemovePane(paneIdx);    // 窗格空了就合并掉
        SaveFavorites();        // panes= 记录要跟实际窗格数保持一致
    }
    SelectRightTab((index < tabs_.size()) ? index : tabs_.size() - 1);
    UpdateRightTabLabels();
    Layout();
}


void MainWindow::InsertColumns(HWND list)
{
    auto add = [&](int idx, const wchar_t* text, int w) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, LVCFMT_LEFT, w, const_cast<LPWSTR>(text) };
        c.iSubItem = idx;
        ListView_InsertColumn(list, idx, &c);
    };
    add(COL_NAME, L"名称", 300);
    add(COL_TYPE, L"类型", 140);
    add(COL_SIZE, L"大小", 110);
    add(COL_MTIME, L"修改日期", 160);
}

// ---------------------------------------------------------------------------
// 导航
// ---------------------------------------------------------------------------
// 规范化路径：折叠连续分隔符（D:\\Amlogic -> D:\Amlogic）、统一为 \、
// 去掉结尾多余 \（根盘符 "C:\" 除外）
static std::wstring NormalizePath(const std::wstring& in)
{
    // 保险：截断嵌入的 '\0'（转发路径异常时末尾可能带 '\0'，后续拼 pattern 会失效）
    std::wstring s(in.c_str());
    // 去掉首尾的引号与空白（外部启动器可能传 "D:\dir" + 空格 之类的形式）
    {
        size_t b = s.find_first_not_of(L" \t\"\"");
        size_t e = s.find_last_not_of(L" \t\"\"");
        if (b == std::wstring::npos) return {};
        s = s.substr(b, e - b + 1);
    }
    if (s.empty()) return s;
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') { // UNC 前缀保留
        out += L"\\\\";
        i = 2;
    }
    bool prevSlash = false;
    for (; i < s.size(); ++i) {
        if (s[i] == L'\\' || s[i] == L'/') {
            if (!prevSlash) out += L'\\';
            prevSlash = true;
        } else {
            out += s[i];
            prevSlash = false;
        }
    }
    if (out.size() > 3 && out.back() == L'\\')
        out.pop_back();
    return out;
}

void MainWindow::Navigate(const std::wstring& rawPath, bool addHistory)
{
    std::wstring path = NormalizePath(rawPath);
    TabState& t = CurTab();
    t.dir = path;
    t.curPage = 0;
    t.pageItems.clear();
    SetWindowTextW(address_, path.c_str());

    if (addHistory) {
        if (!t.history.empty() && t.histPos >= 0 && t.history[t.histPos] == path) {
            // same
        } else {
            t.history.erase(t.history.begin() + t.histPos + 1, t.history.end());
            t.history.push_back(path);
            t.histPos = static_cast<int>(t.history.size()) - 1;
        }
    }

    t.pages->OpenDirectory(path, EffectivePageSize());
    ListView_SetItemCountEx(CurList(), 0, 0);
    UpdateStatusBar();
    UpdatePaginationBar();
    UpdateRightTabLabels();
}

// 目录树 / 收藏 的跳转入口。
// 当前分页被锁定时不改动它，而是在同一窗格里新开一个分页来打开目标目录。
void MainWindow::NavigateFromSidebar(const std::wstring& path)
{
    if (path.empty()) return;
    if (!CurTab().locked) { Navigate(path); return; }
    size_t paneIdx = activePane_;
    AddRightTab(false, paneIdx);   // 新建并激活分页（先不跳默认目录）
    Navigate(path);                // 再在新分页里打开目标目录
}

// 目录树中键点击：无论当前分页是否锁定，总是在当前活动窗格新开一个分页打开目录
void MainWindow::OpenDirInNewTab(const std::wstring& path)
{
    if (path.empty()) return;
    AddRightTab(false, activePane_);   // 新建并激活分页（先不跳默认目录）
    Navigate(path);                    // 再在新分页里打开目标目录
}

void MainWindow::RefreshList()
{
    TabState& t = CurTab();
    t.pageItems.clear();
    ListView_SetItemCountEx(CurList(), 0, 0);
    t.pages->RequestPage(t.curPage); // 可能命中缓存，也可能后台加载
    if (t.pages->TryGetPage(t.curPage, t.pageItems)) {
        ApplyCurrentSort(); // 套用已保存的排序
        ListView_SetItemCountEx(CurList(), t.pageItems.size(), LVSICF_NOINVALIDATEALL);
        ListView_RedrawItems(CurList(), 0, static_cast<int>(t.pageItems.size()) - 1);
    }
    UpdateStatusBar();
    UpdatePaginationBar();
}

void MainWindow::RefreshListFromDisk()
{
    // 删除/粘贴/改名后页面数据已过期：清掉分页缓存再刷新，
    // 否则 RequestPage 直接命中旧缓存，列表显示的还是操作前的内容
    CurTab().pages->Invalidate();
    RefreshList();
}

void MainWindow::OnPageLoaded()
{
    // 后台加载完成后：如果当前分页还没内容就填充
    TabState& t = CurTab();
    if (t.pageItems.empty() && t.pages->TryGetPage(t.curPage, t.pageItems)) {
        ApplyCurrentSort(); // 套用已保存的排序
        ListView_SetItemCountEx(CurList(), t.pageItems.size(), LVSICF_NOINVALIDATEALL);
        ListView_RedrawItems(CurList(), 0, static_cast<int>(t.pageItems.size()) - 1);
    }
    t.pages->PrefetchAround(t.curPage);
    UpdateStatusBar();
    UpdatePaginationBar();
}

void MainWindow::UpdateStatusBar()
{
    wchar_t buf[160];
    unsigned long long total = CurTab().pages->TotalCount();
    if (total > 0 && !paginationEnabled_)
        wsprintfW(buf, L"共 %I64u 项（未分页）", total);
    else if (total > 0)
        wsprintfW(buf, L"共 %I64u 项   第 %I64u / %zu 页   每页 %zu 项",
            total, static_cast<unsigned long long>(CurTab().curPage + 1), CurTab().pages->PageCount(), pageSize_);
    else
        wsprintfW(buf, L"正在统计 %s ...", CurTab().dir.c_str());
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(buf));
}

void MainWindow::UpdatePaginationBar()
{
    if (!paginationEnabled_) return;   // 分页栏整条已隐藏
    wchar_t buf[80];
    unsigned long long total = CurTab().pages->TotalCount();
    if (total > 0)
        wsprintfW(buf, L"第 %I64u / %zu 页", static_cast<unsigned long long>(CurTab().curPage + 1), CurTab().pages->PageCount());
    else
        wsprintfW(buf, L"加载中...");
    SetWindowTextW(pagerLabel_, buf);
    EnableWindow(pagerFirst_, CurTab().curPage > 0);
    EnableWindow(pagerPrev_, CurTab().curPage > 0);
    EnableWindow(pagerNext_, total > 0 && (CurTab().curPage + 1) < CurTab().pages->PageCount());
    EnableWindow(pagerLast_, total > 0 && (CurTab().curPage + 1) < CurTab().pages->PageCount());
}

// 点击表头排序（作用于当前页条目）
void MainWindow::OnColumnClick(int col)
{
    if (col == sortCol_) sortAsc_ = !sortAsc_;
    else { sortCol_ = col; sortAsc_ = true; }
    SaveFavorites(); // 记住排序设置
    ApplyCurrentSort();
}

// 按 sortCol_/sortAsc_ 排序当前页并重绘（页面加载后也调用，保持排序生效）
void MainWindow::ApplyCurrentSort()
{
    auto strLess = [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    };
    int col = sortCol_;
    bool asc = sortAsc_;
    TabState& t = CurTab();
    std::sort(t.pageItems.begin(), t.pageItems.end(), [&](const FileEntry& a, const FileEntry& b) {
        if (a.isFolder != b.isFolder) return a.isFolder > b.isFolder; // 文件夹始终在前
        switch (col) {
        case COL_NAME:  return asc ? strLess(a.name, b.name) : strLess(b.name, a.name);
        case COL_TYPE:  return asc ? strLess(a.typeName, b.typeName) : strLess(b.typeName, a.typeName);
        case COL_SIZE:  return asc ? a.size < b.size : a.size > b.size;
        case COL_MTIME: return asc ? FileTimeToUInt64(a.writeTime) < FileTimeToUInt64(b.writeTime)
                                   : FileTimeToUInt64(a.writeTime) > FileTimeToUInt64(b.writeTime);
        }
        return false;
    });
    if (!CurTab().pageItems.empty())
        ListView_RedrawItems(CurList(), 0, static_cast<int>(CurTab().pageItems.size()) - 1);
}

// ---------------------------------------------------------------------------
// 虚拟 ListView：LVN_GETDISPINFO 时才取数据，几十个可见项，与总页数无关
// ---------------------------------------------------------------------------
int MainWindow::EnsureIcon(FileEntry& e)
{
    if (e.iconIndex < 0)
        e.iconIndex = shell::SysIconIndexForEntry(e.path, e.isFolder);
    return e.iconIndex >= 0 ? e.iconIndex : I_IMAGENONE;
}

std::wstring FormatSize(unsigned long long sz)
{
    wchar_t buf[64];
    if (sz < 1024) swprintf_s(buf, L"%I64u B", sz);
    else if (sz < 1024ull * 1024) swprintf_s(buf, L"%.1f KB", sz / 1024.0);
    else if (sz < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", sz / (1024.0 * 1024));
    else swprintf_s(buf, L"%.2f GB", sz / (1024.0 * 1024 * 1024));
    return buf;
}

std::wstring FormatTime(const FILETIME& ft)
{
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring MainWindow::CurrentPagePath(int item) const
{
    if (item < 0 || item >= static_cast<int>(CurTab().pageItems.size())) return {};
    return CurTab().pageItems[item].path;
}

bool MainWindow::SelectedPath(std::wstring& out) const
{
    int sel = ListView_GetNextItem(CurList(), -1, LVNI_SELECTED);
    if (sel < 0) return false;
    out = CurrentPagePath(sel);
    return !out.empty();
}

std::vector<std::wstring> MainWindow::SelectedPaths() const
{
    std::vector<std::wstring> out;
    int sel = -1;
    for (;;) {
        sel = ListView_GetNextItem(CurList(), sel, LVNI_SELECTED);
        if (sel < 0) break;
        std::wstring p = CurrentPagePath(sel);
        if (!p.empty()) out.push_back(std::move(p));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 树
// ---------------------------------------------------------------------------
// 打开外部传入的目录或文件：目录直接导航；文件导航到其所在目录
void MainWindow::OpenTarget(const std::wstring& rawPath)
{
    std::wstring path = NormalizePath(rawPath);
    if (path.empty()) return;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return; // 不存在，忽略
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        Navigate(path);
    } else {
        size_t s = path.find_last_of(L'\\');
        if (s != std::wstring::npos && s > 0) Navigate(path.substr(0, s));
    }
}

// ---------------------------------------------------------------------------
// 收藏目录
// ---------------------------------------------------------------------------
static std::wstring FavoritesFilePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    size_t s = dir.find_last_of(L'\\');
    if (s != std::wstring::npos) dir.resize(s);
    return dir + L"\\favorites.txt";
}

std::wstring MainWindow::SessionFilePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    size_t s = dir.find_last_of(L'\\');
    if (s != std::wstring::npos) dir.resize(s);
    return dir + L"\\session.txt";
}

// 整个文件一次性读入并解析（不再用 1024 定长缓冲逐行 fgets——
// 长路径的 UTF-8 多字节字符被拦腰截断会产生坏收藏项）。
// 返回文件是否成功打开（内容可能为空）；out 收收藏路径。
bool MainWindow::ReadFavoritesFromDisk(std::vector<std::wstring>& out)
{
    out.clear();
    std::wstring path = FavoritesFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;

    std::string data;
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, r);
    fclose(f);

    // 去掉 UTF-8 BOM
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);

    // 按行解析；配置行 sort=/panes=/tri= 可出现在任意位置
    size_t pos = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0);
        if (wlen <= 0) continue; // 无效 UTF-8 行跳过，不产生坏数据
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), (int)line.size(), w.data(), wlen);
        if (w.rfind(L"sort=", 0) == 0) {
            int col = 0, asc = 1;
            swscanf_s(w.c_str() + 5, L"%d,%d", &col, &asc);
            if (col >= 0 && col <= 3) { sortCol_ = col; sortAsc_ = asc != 0; }
            continue;
        }
        if (w.rfind(L"panes=", 0) == 0) {   // 上次退出时的窗格数量 1~4
            int n = 1;
            swscanf_s(w.c_str() + 6, L"%d", &n);
            savedPaneCount_ = (n >= 1 && n <= 4) ? n : 1;
            continue;
        }
        if (w.rfind(L"tri=", 0) == 0) {     // 上次的三窗格形态：0=品字形 1=倒品字形
            int t = 1;
            swscanf_s(w.c_str() + 4, L"%d", &t);
            triLayout_ = t ? 1 : 0;
            continue;
        }
        out.push_back(std::move(w));
    }
    return true;
}

void MainWindow::LoadFavorites()
{
    // 先解析到临时列表，成功才替换内存——文件打不开/解析失败时
    // 绝不清空现有收藏（旧版在这里 clear，之后任意一次保存就把空列表写盘，
    // 收藏永久丢失）。
    std::vector<std::wstring> parsed;
    if (ReadFavoritesFromDisk(parsed))
        favorites_ = std::move(parsed);
    RefreshFavoritesList();
}

void MainWindow::SaveFavorites()
{
    // 防丢失保护：内存列表为空但磁盘上还有收藏时（加载失败/时序竞态），
    // 先从磁盘取回，避免把收藏写没。
    if (favorites_.empty()) {
        std::vector<std::wstring> disk;
        if (ReadFavoritesFromDisk(disk) && !disk.empty())
            favorites_ = std::move(disk);
    }
    SaveFavoritesCore(false);
}

void MainWindow::SaveFavoritesCore(bool allowEmptyFavorites)
{
    // 非显式删除场景下拒绝把有收藏的文件覆盖成空文件
    if (!allowEmptyFavorites && favorites_.empty()) {
        std::vector<std::wstring> disk;
        if (ReadFavoritesFromDisk(disk) && !disk.empty())
            return; // 磁盘还有收藏且内存为空：保持磁盘原样
    }
    std::wstring path = FavoritesFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    // 写 UTF-8 BOM + 配置行 + UTF-8 行
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, f);
    auto putLine = [&](const std::wstring& s) {
        char buf[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    buf, sizeof(buf) - 2, nullptr, nullptr);
        if (n > 0) { buf[n++] = '\n'; fwrite(buf, 1, n, f); }
    };
    putLine(L"sort=" + std::to_wstring(sortCol_) + L"," + std::to_wstring(sortAsc_ ? 1 : 0));
    putLine(L"panes=" + std::to_wstring(panes_.size())); // 下次启动沿用窗格数量
    putLine(L"tri=" + std::to_wstring(triLayout_));      // 下次启动沿用品字形态
    for (const auto& dir : favorites_)
        putLine(dir);
    fclose(f);
}

void MainWindow::RefreshFavoritesList()
{
    ListView_DeleteAllItems(favList_);
    for (int i = 0; i < (int)favorites_.size(); ++i) {
        // 显示最后一段目录名
        const std::wstring& dir = favorites_[i];
        std::wstring name = dir;
        size_t s = name.find_last_of(L'\\');
        if (s + 1 == name.size()) {           // 以 \ 结尾（盘符 C:\）
            name.pop_back();
            s = name.find_last_of(L'\\');
        }
        if (s != std::wstring::npos) name = name.substr(s + 1);

        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = i;
        it.lParam = reinterpret_cast<LPARAM>(new std::wstring(dir));
        it.pszText = const_cast<LPWSTR>(name.c_str());
        it.iImage = shell::SysIconIndexForEntry(dir, true);
        ListView_InsertItem(favList_, &it);
    }
}

void MainWindow::OnAddFavorite()
{
    if (CurTab().dir.empty()) return;
    // 去重
    for (const auto& d : favorites_)
        if (_wcsicmp(d.c_str(), CurTab().dir.c_str()) == 0) return;
    favorites_.push_back(CurTab().dir);
    SaveFavorites();
    RefreshFavoritesList();
}

void MainWindow::OnRemoveFavorite()
{
    int sel = ListView_GetNextItem(favList_, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)favorites_.size()) return;
    auto* p = reinterpret_cast<std::wstring*>([&] {
        LVITEMW it{}; it.iItem = sel; it.mask = LVIF_PARAM;
        ListView_GetItem(favList_, &it);
        return it.lParam;
    }());
    delete p;
    favorites_.erase(favorites_.begin() + sel);
    // 显式删除：允许写空文件（绕过防丢失保护，否则删掉的最后一条会被“恢复”）
    SaveFavoritesCore(favorites_.empty());
    RefreshFavoritesList();
}

// ---------------------------------------------------------------------------
// 键盘操作
// ---------------------------------------------------------------------------
void MainWindow::OnDelete(bool toRecycleBin)
{
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    if (shell::ExecuteFileOpMulti(hwnd_, shell::FileOp::Delete, paths, L"", toRecycleBin))
        RefreshListFromDisk();
}

// 重命名对话框窗口过程：IDOK/IDCANCEL -> 记录结果并销毁
static bool g_renameConfirmed = false;
static std::wstring g_renameText;

static LRESULT CALLBACK RenameDlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            wchar_t buf[MAX_PATH] = {};
            HWND edit = GetDlgItem(h, 1);
            if (edit) GetWindowTextW(edit, buf, MAX_PATH);
            g_renameText = buf;
            g_renameConfirmed = (LOWORD(wp) == IDOK);
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void MainWindow::OnRename()
{
    std::wstring path;
    if (!SelectedPath(path)) {
        WriteAppLog(L"RENAME request ignored: no selected item in active pane");
        return;
    }
    WriteAppLog((L"RENAME requested from active pane: " + path).c_str());
    RenamePath(path);
}

void MainWindow::RenamePath(const std::wstring& path)
{
    if (path.empty()) {
        WriteAppLog(L"RENAME aborted: empty source path");
        return;
    }
    std::wstring oldName = path.substr(path.find_last_of(L'\\') + 1);
    WriteAppLog((L"RENAME dialog opening: path=" + path +
                 L", currentName=" + oldName).c_str());

    const wchar_t* DLG_CLASS = L"PagedExplorerRenameBox";
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = RenameDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = DLG_CLASS;
        cls = RegisterClassW(&wc);
        if (!cls && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            cls = 1;
    }
    if (!cls) {
        WriteAppLog((L"RENAME dialog class registration failed: error=" +
                     std::to_wstring(GetLastError())).c_str());
        MessageBoxW(hwnd_, L"无法创建重命名对话框。", L"重命名",
                    MB_OK | MB_ICONERROR);
        return;
    }

    g_renameConfirmed = false;
    g_renameText.clear();

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, DLG_CLASS, L"重命名",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 420, 112, hwnd_, nullptr, hInst, nullptr);
    if (!dlg) {
        WriteAppLog((L"RENAME dialog creation failed: error=" +
                     std::to_wstring(GetLastError())).c_str());
        MessageBoxW(hwnd_, L"无法打开重命名对话框。", L"重命名",
                    MB_OK | MB_ICONERROR);
        return;
    }
    // 居中到主窗口
    RECT rm; GetWindowRect(hwnd_, &rm);
    SetWindowPos(dlg, nullptr,
        (rm.left + rm.right) / 2 - 210, (rm.top + rm.bottom) / 2 - 56,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, oldName.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        10, 14, 392, 24, dlg, (HMENU)1, hInst, nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    // 选中主名（不含扩展名）
    size_t dot = oldName.find_last_of(L'.');
    if (dot != 0 && dot != std::wstring::npos)
        SendMessageW(edit, EM_SETSEL, 0, dot);

    HWND ok = CreateWindowExW(0, WC_BUTTONW, L"确定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON,
        208, 48, 92, 26, dlg, (HMENU)IDOK, hInst, nullptr);
    CreateWindowExW(0, WC_BUTTONW, L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        310, 48, 92, 26, dlg, (HMENU)IDCANCEL, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    SendMessageW(dlg, DM_SETDEFID, IDOK, 0);

    ShowWindow(dlg, SW_SHOW);
    SetFocus(edit);
    EnableWindow(hwnd_, FALSE); // 禁用主窗口，模态

    // 模态消息循环：对话框销毁即结束
    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)
            && (GetParent(m.hwnd) == dlg || m.hwnd == dlg)) {
            SendMessageW(dlg, WM_COMMAND,
                MAKEWPARAM(m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0), 0);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(hwnd_, TRUE);
    SetFocus(CurList());

    if (!g_renameConfirmed) {
        WriteAppLog((L"RENAME dialog cancelled: path=" + path).c_str());
        return;
    }
    if (g_renameText.empty() || g_renameText == oldName) {
        WriteAppLog((L"RENAME no-op: path=" + path +
                     L", enteredName=" + g_renameText).c_str());
        return;
    }

    WriteAppLog((L"RENAME operation starting: source=" + path +
                 L", destinationName=" + g_renameText).c_str());
    if (shell::ExecuteFileOp(hwnd_, shell::FileOp::Rename, path, g_renameText)) {
        WriteAppLog((L"RENAME operation succeeded: source=" + path +
                     L", destinationName=" + g_renameText).c_str());
        RefreshListFromDisk();
    } else {
        WriteAppLog((L"RENAME operation failed: source=" + path +
                     L", destinationName=" + g_renameText).c_str());
        MessageBoxW(hwnd_, L"重命名失败。请确认文件仍存在且目标名称可用。",
                    L"重命名", MB_OK | MB_ICONERROR);
    }
}

// 把文件路径列表放进剪贴板（CF_HDROP + Preferred DropEffect），与资源管理器
// 完全互通：本程序/资源管理器里复制或剪切的文件，两边都能互相粘贴
static void SetFileClipboard(HWND hwnd, const std::vector<std::wstring>& paths, bool cut)
{
    if (paths.empty() || !OpenClipboard(hwnd)) return;
    EmptyClipboard();

    // CF_HDROP：DROPFILES 头 + 双 '\0' 结尾的宽字符路径串
    SIZE_T chars = 0;
    for (const auto& p : paths) chars += p.size() + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE,
                            sizeof(DROPFILES) + (chars + 1) * sizeof(wchar_t));
    if (h) {
        auto* df = static_cast<DROPFILES*>(GlobalLock(h));
        if (df) {
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            auto* w = reinterpret_cast<wchar_t*>(df + 1);
            for (const auto& p : paths) {
                size_t n = p.size() + 1;
                wmemcpy(w, p.c_str(), n);
                w += n;
            }
            *w = L'\0';                       // 列表以双 '\0' 结束
            GlobalUnlock(h);
            if (!SetClipboardData(CF_HDROP, h)) GlobalFree(h); // 成功后归系统所有
        } else {
            GlobalFree(h);
        }
    }

    // 剪切/复制语义
    UINT pe = RegisterClipboardFormatW(L"Preferred DropEffect");
    HGLOBAL he = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (he) {
        auto* effect = static_cast<DWORD*>(GlobalLock(he));
        if (effect) {
            *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(he);
            if (!SetClipboardData(pe, he)) GlobalFree(he);
        } else {
            GlobalFree(he);
        }
    }
    CloseClipboard();
}

void MainWindow::OnClipboard(bool cut)
{
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    SetFileClipboard(hwnd_, paths, cut);
}

void MainWindow::OnPaste()
{
    std::vector<std::wstring> srcs;
    bool move = false;

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        // 标准文件剪贴板：本程序/资源管理器复制或剪切的内容都走这里
        if (!OpenClipboard(hwnd_)) return;
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            auto* drop = static_cast<HDROP>(GlobalLock(h));
            if (drop) {
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    UINT len = DragQueryFileW(drop, i, nullptr, 0); // 不含结尾 '\0'
                    std::wstring p(len, L'\0');
                    if (DragQueryFileW(drop, i, p.data(), len + 1) > 0)
                        srcs.push_back(std::move(p));
                }
                UINT pe = RegisterClipboardFormatW(L"Preferred DropEffect");
                HANDLE he = pe ? GetClipboardData(pe) : nullptr;
                if (he) {
                    auto* effect = static_cast<DWORD*>(GlobalLock(he));
                    if (effect) {
                        if (*effect & DROPEFFECT_MOVE) move = true;
                        GlobalUnlock(he);
                    }
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        // 兼容回退：剪贴板是文本且恰好是一个存在的路径 -> 复制该文件
        if (!OpenClipboard(hwnd_)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        std::wstring src = h ? static_cast<const wchar_t*>(GlobalLock(h)) : L"";
        if (h) GlobalUnlock(h);
        CloseClipboard();
        size_t b = src.find_first_not_of(L" \t\r\n\"");
        size_t e = src.find_last_not_of(L" \t\r\n\"");
        if (b != std::wstring::npos) src = src.substr(b, e - b + 1);
        if (!src.empty() &&
            GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES)
            srcs.push_back(src);
    }

    if (srcs.empty()) return;
    if (shell::ExecuteFileOpMulti(hwnd_,
                                  move ? shell::FileOp::Move : shell::FileOp::Copy,
                                  srcs, CurTab().dir, true))
        RefreshListFromDisk();
}

DWORD MainWindow::HandleFileDrop(HWND list, const std::vector<std::wstring>& paths,
                                 DWORD effect)
{
    int paneIndex = PaneOfList(list);
    if (paneIndex < 0 || paths.empty() ||
        (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE))
        return DROPEFFECT_NONE;

    SelectPane(static_cast<size_t>(paneIndex));
    shell::FileOp operation = effect == DROPEFFECT_MOVE
        ? shell::FileOp::Move : shell::FileOp::Copy;
    if (!shell::ExecuteFileOpMulti(hwnd_, operation, paths, CurTab().dir, true)) {
        WriteAppLog(L"File drop operation failed");
        return DROPEFFECT_NONE;
    }
    RefreshListFromDisk();
    return effect;
}

// 地址栏子类化：回车时向主窗口发 EN_RETURN 通知
static LRESULT CALLBACK AddressProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(GetParent(h), GWLP_USERDATA));
        if (self)
            SendMessageW(GetParent(h), WM_COMMAND,
                         MAKEWPARAM(IDC_ADDRESS, EN_ADDR_RETURN), reinterpret_cast<LPARAM>(h));
        return 0;
    }
    if (m == WM_CHAR && wp == VK_RETURN) return 0; // 避免叮声
    return CallWindowProcW(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA)),
                           h, m, wp, lp);
}

// ---------------------------------------------------------------------------
// 分页拖拽：子类化窗格 tab 控件，按住 tab 头拖到另一窗格即移动
// （拖拽状态存主窗口，tab 过程通过 GWLP_USERDATA 找回 MainWindow）
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SideTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!orig) return DefWindowProcW(h, m, wp, lp);
    if (m == WM_COMMAND || m == WM_NOTIFY) {
        HWND parent = GetParent(h);
        if (parent) return SendMessageW(parent, m, wp, lp);
    }
    return CallWindowProcW(orig, h, m, wp, lp);
}

static LRESULT CALLBACK PaneTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (!IsWindow(h)) return 0; // 窗格被销毁后可能还有残余消息，别再往下走
    WNDPROC orig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!orig) return DefWindowProcW(h, m, wp, lp);
    if (m == WM_COMMAND || m == WM_NOTIFY) {
        HWND parent = GetParent(h);
        if (parent) return SendMessageW(parent, m, wp, lp);
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(GetParent(h), GWLP_USERDATA));
    if (self) return self->PaneTabHandler(h, m, wp, lp, orig);
    return CallWindowProcW(orig, h, m, wp, lp);
}

LRESULT MainWindow::PaneTabHandler(HWND h, UINT m, WPARAM wp, LPARAM lp, WNDPROC orig)
{
    // Hit zones overlap pane tabs; forward those mouse events because the parent won't receive them.
    auto isSplitterHit = [&](POINT pt) {
        ClientToScreen(h, &pt);
        ScreenToClient(hwnd_, &pt);
        if (pt.y < splitTop_ || pt.y > splitBot_) return false;
        if (rowSplitY_ >= 0 &&
            pt.y >= rowSplitY_ - kSplitHit && pt.y <= rowSplitY_ + kSplitHit &&
            !paneRects_.empty()) {
            int left = paneRects_.front().left;
            int right = paneRects_.front().right;
            for (const RECT& r : paneRects_) {
                if (r.left < left) left = r.left;
                if (r.right > right) right = r.right;
            }
            if (pt.x >= left && pt.x <= right) return true;
        }
        for (const auto& pr : splitPairs_) {
            int edge = paneRects_[pr.first].right;
            if (pt.x >= edge && pt.x <= edge + kSplitHit &&
                pt.y >= paneRects_[pr.first].top && pt.y <= paneRects_[pr.first].bottom)
                return true;
        }
        return pt.x >= sideWidth_ && pt.x <= sideWidth_ + kSplitHit;
    };

    if (m == WM_LBUTTONDOWN) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (isSplitterHit(pt)) {
            ClientToScreen(h, &pt);
            ScreenToClient(hwnd_, &pt);
            SendMessageW(hwnd_, WM_LBUTTONDOWN, wp, MAKELPARAM(pt.x, pt.y));
            return 0;
        }
    } else if (m == WM_SETCURSOR) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        if (isSplitterHit(pt)) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            ClientToScreen(h, &pt);
            ScreenToClient(hwnd_, &pt);
            if (rowSplitY_ >= 0 &&
                pt.y >= rowSplitY_ - kSplitHit && pt.y <= rowSplitY_ + kSplitHit) {
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            }
            return TRUE;
        }
    }

    switch (m) {
    case WM_PAINT: {
        // 当前焦点窗格：分页栏整行底色涂淡粉绿。先让控件按主题画完，
        // 再只覆盖分页头之外的空隙（行背景），分页头本身保持原样。
        LRESULT r = CallWindowProcW(orig, h, m, wp, lp);
        int pi = PaneOfTab(h);
        if (pi < 0 || (size_t)pi != activePane_) return r;
        HDC hdc = GetDC(h);
        if (!hdc) return r;
        RECT rc{};
        GetClientRect(h, &rc);
        int cnt = (int)SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
        int rowH = 24;
        HRGN tabs = CreateRectRgn(0, 0, 0, 0);
        for (int i = 0; i < cnt; ++i) {
            RECT ti{};
            if (SendMessageW(h, TCM_GETITEMRECT, (WPARAM)i, (LPARAM)&ti)) {
                rowH = ti.bottom + 3;              // 分页头行的高度（含底部边距）
                HRGN tr = CreateRectRgn(ti.left, ti.top, ti.right, ti.bottom);
                CombineRgn(tabs, tabs, tr, RGN_OR);
                DeleteObject(tr);
            }
        }
        if (rowH > rc.bottom) rowH = rc.bottom;
        HRGN full = CreateRectRgn(rc.left, rc.top, rc.right, rc.top + rowH);
        HRGN gaps = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(gaps, full, tabs, RGN_DIFF);
        HBRUSH br = CreateSolidBrush(kActiveTabStripColor);
        FillRgn(hdc, gaps, br);
        DeleteObject(br);
        DeleteObject(gaps);
        DeleteObject(full);
        DeleteObject(tabs);
        ReleaseDC(h, hdc);
        return r;
    }
    case WM_LBUTTONDBLCLK: {
        // 分页栏空白区双击 = 新建分页。tab 控件类带 CS_DBLCLKS，双击的第二次
        // 点击以本消息到达。不在这里直接 AddRightTab：那会在 tab 控件自己的
        // 窗口过程里 TCM_INSERTITEM + Layout(对它 SetWindowPos)，重入 comctl32
        // 内部状态会把它点崩（COMCTL32 c000041d），丢给主窗口消息循环处理。
        TCHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = (int)SendMessageW(h, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
        int pi = PaneOfTab(h);
        if (idx < 0 && pi >= 0) {
            lastBlankClickTime_ = 0;    // 收尾，避免再触发一次时间差检测
            if (!PostMessageW(hwnd_, WM_APP_ADD_TAB, static_cast<WPARAM>(pi), 0))
                WriteAppLog(L"ADD_TAB post failed after blank-area double-click");
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        TCHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = (int)SendMessageW(h, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
        int pi = PaneOfTab(h);
        if (idx >= 0) {
            // 记录拖拽起点，但不拦截：继续走默认过程完成 tab 切换
            tabDragActive_ = true;
            tabDragPane_ = pi;
            tabDragIndex_ = idx;
            SetCapture(h);
        } else if (pi >= 0) {
            // 分页栏空白区域：双击新建分页（不依赖 CS_DBLCLKS，自己测时间差）。
            // 同样延后到主窗口处理，原因同 WM_LBUTTONDBLCLK 分支。
            DWORD now = GetMessageTime();
            if (now - lastBlankClickTime_ <= (DWORD)GetDoubleClickTime() &&
                abs(ht.pt.x - lastBlankClickPt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                abs(ht.pt.y - lastBlankClickPt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK)) {
                lastBlankClickTime_ = 0;
                if (!PostMessageW(hwnd_, WM_APP_ADD_TAB, static_cast<WPARAM>(pi), 0))
                    WriteAppLog(L"ADD_TAB post failed after blank-area double-click");
                return 0;
            }
            lastBlankClickTime_ = now;
            lastBlankClickPt_ = ht.pt;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (tabDragActive_ && (wp & MK_LBUTTON) &&
            tabDragPane_ >= 0 && tabDragPane_ < (int)panes_.size()) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
            ScreenToClient(hwnd_, &pt);
            // 落在其它窗格区域 -> 移动分页（整块矩形判定：上下排也能分清）
            for (size_t i = 0; i < panes_.size() && i < paneRects_.size(); ++i) {
                if ((int)i == tabDragPane_) continue;
                if (PtInRect(&paneRects_[i], pt)) {
                    size_t tabIndex = SIZE_MAX;
                    if (tabDragIndex_ >= 0 && (size_t)tabDragIndex_ < panes_[tabDragPane_].tabs.size())
                        tabIndex = panes_[tabDragPane_].tabs[tabDragIndex_];
                    tabDragActive_ = false; // 拖拽状态先收尾
                    if (GetCapture() == h) ReleaseCapture();
                    if (tabIndex != SIZE_MAX) {
                        // 移动可能连带销毁这个 tab（源窗格只有这一个分页），
                        // 不能在 tab 自己的窗口过程中 DestroyWindow，延后到主窗口处理
                        pendingMoveTab_ = tabIndex;
                        pendingMovePane_ = (int)i;
                        PostMessageW(hwnd_, WM_APP_MOVE_TAB, 0, 0);
                    }
                    return 0;
                }
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (tabDragActive_) {
            tabDragActive_ = false;
            if (GetCapture() == h) ReleaseCapture();
        }
        break;
    }
    case WM_RBUTTONDOWN: {  // 右键分页标题 -> 关闭/关闭其他/关闭右边/锁定
        TCHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = (int)SendMessageW(h, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
        if (idx >= 0) {
            POINT pt = ht.pt;
            ClientToScreen(h, &pt);
            TabContextMenu(h, idx, pt);
            return 0;
        }
        break;              // 标题空白处右键不管，交给默认过程
    }
    case WM_MBUTTONDOWN: {  // 中键点击分页标题 -> 关闭该分页
        if (tabDragActive_) {           // 中键不参与拖拽，先收尾
            tabDragActive_ = false;
            if (GetCapture() == h) ReleaseCapture();
        }
        TCHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = (int)SendMessageW(h, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
        int pi = PaneOfTab(h);
        if (idx >= 0 && pi >= 0 && (size_t)idx < panes_[pi].tabs.size()) {
            // 关分页可能连带销毁这个窗格，不能在 tab 自己的窗口过程中 DestroyWindow，
            // 丢给主窗口下一次消息循环处理
            pendingCloseTab_ = panes_[pi].tabs[idx];
            PostMessageW(hwnd_, WM_APP_CLOSE_TAB, 0, 0);
        }
        return 0;   // 吞掉中键，别触发系统的滚动热点
    }
    case WM_CAPTURECHANGED:
        tabDragActive_ = false;
        break;
    }
    return CallWindowProcW(orig, h, m, wp, lp);
}

// 依据激活窗格“最后一个分页头”的实际位置摆放 “+” 按钮。
// 分页标题会随导航变化（长度不同 -> tab 宽度变化），且标题是异步更新的，
// 所以不能只在 Layout() 里算一次，否则按钮会停在旧宽度处、与 tab 头重叠。
void MainWindow::UpdateNewTabButtons()
{
    for (size_t i = 0; i < panes_.size(); ++i) {
        Pane& p = panes_[i];

        // 右上角“▾”外部工具按钮：贴本窗格 tab 条的最右端
        int toolsLeft = -1;
        if (p.btnTools != nullptr && p.tab != nullptr) {
            RECT tc{};
            if (GetClientRect(p.tab, &tc) && tc.right > 30) {
                POINT tr{ tc.right - 26, 1 };
                MapWindowPoints(p.tab, hwnd_, &tr, 1);
                SetWindowPos(p.btnTools, HWND_TOP, tr.x, tr.y, 24, 20,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                toolsLeft = tr.x;
            }
        }

        if (p.btnNewTab == nullptr || p.tab == nullptr) continue;
        int cnt = (int)SendMessageW(p.tab, TCM_GETITEMCOUNT, 0, 0);
        if (cnt <= 0) { ShowWindow(p.btnNewTab, SW_HIDE); continue; }
        RECT rl{};
        if (!SendMessageW(p.tab, TCM_GETITEMRECT, cnt - 1, reinterpret_cast<LPARAM>(&rl))) {
            ShowWindow(p.btnNewTab, SW_HIDE); continue;
        }
        // tab 客户区坐标 -> 主窗口坐标（不手写边框偏移）
        POINT tl{ rl.left, rl.top }, br{ rl.right, rl.bottom };
        MapWindowPoints(p.tab, hwnd_, &tl, 1);
        MapWindowPoints(p.tab, hwnd_, &br, 1);
        int x = br.x + 6;                       // 紧贴最后一个 tab 头右侧
        int y = tl.y + ((br.y - tl.y) - 22) / 2; // 与该 tab 头垂直居中
        p.lastTabRight = x;
        // tab 太多快顶到“▾”按钮时收起“+”（分页栏放不下就别叠上去）
        if (toolsLeft >= 0 && x + 26 > toolsLeft - 2) {
            ShowWindow(p.btnNewTab, SW_HIDE);
            continue;
        }
        SetWindowPos(p.btnNewTab, HWND_TOP, x, y, 26, 22,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// ---------------------------------------------------------------------------
// 窗格右上角“▾”外部工具菜单
// ---------------------------------------------------------------------------
static std::wstring ToolsFilePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir(buf);
    size_t s = dir.find_last_of(L'\\');
    if (s != std::wstring::npos) dir.resize(s);
    return dir + L"\\tools.txt";
}

struct PaneTool { std::wstring name, cmdline; };

// 解析工具列表文本：每行 名称|命令行，# 开头为注释
static std::vector<PaneTool> ParsePaneTools(const std::wstring& text)
{
    std::vector<PaneTool> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        std::wstring line = text.substr(pos,
            (e == std::wstring::npos ? text.size() : e) - pos);
        if (e == std::wstring::npos) pos = text.size() + 1;
        else pos = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t bar = line.find(L'|');
        if (bar == std::wstring::npos || bar == 0 || bar + 1 >= line.size()) continue;
        out.push_back({ line.substr(0, bar), line.substr(bar + 1) });
    }
    return out;
}

// 读外部工具列表；文件不存在时先写入默认配置（CMD / PowerShell / VSCode）
static std::vector<PaneTool> LoadPaneTools()
{
    static const wchar_t* kDefaults =
        L"# 外部工具配置：每行一条，格式：名称|命令行（# 开头的行是注释）\n"
        L"# %DIR% 会替换为按钮所在窗格的当前目录；命令的工作目录也是该目录。\n"
        L"CMD|cmd.exe\n"
        L"PowerShell|powershell.exe\n"
        L"VSCode|cmd /c code \"%DIR%\"\n";

    std::wstring text;
    HANDLE h = CreateFileW(ToolsFilePath().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string utf8;
        char buf[4096];
        DWORD got = 0;
        for (;;) {
            if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            utf8.append(buf, got);
        }
        CloseHandle(h);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                       (int)utf8.size(), nullptr, 0);
        if (wlen > 0) {
            text.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                text.data(), wlen);
        }
    } else {
        // 首次使用：落盘默认配置，方便用户直接改
        text = kDefaults;
        int n = WideCharToMultiByte(CP_UTF8, 0, kDefaults, -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            std::string def(n - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, kDefaults, -1,
                                def.data(), n, nullptr, nullptr);
            HANDLE w = CreateFileW(ToolsFilePath().c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS, 0, nullptr);
            if (w != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(w, def.data(), (DWORD)def.size(), &written, nullptr);
                CloseHandle(w);
            }
        }
    }

    std::vector<PaneTool> tools = ParsePaneTools(text);
    if (tools.empty()) tools = ParsePaneTools(kDefaults); // 文件被清空时兜底
    return tools;
}

// 点击“▾”弹出的下拉菜单：外部工具 + 配置入口
void MainWindow::ShowPaneToolsMenu(size_t paneIdx)
{
    if (paneIdx >= panes_.size() || panes_[paneIdx].btnTools == nullptr) return;
    auto tools = LoadPaneTools();

    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < tools.size(); ++i)
        AppendMenuW(menu, MF_STRING, i + 1, tools[i].name.c_str());
    if (!tools.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1000, L"配置外部工具...");

    RECT br{};
    GetWindowRect(panes_[paneIdx].btnTools, &br);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                               br.left, br.bottom, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd >= 1 && cmd <= (int)tools.size())
        RunPaneTool(paneIdx, tools[cmd - 1].name, tools[cmd - 1].cmdline);
    else if (cmd == 1000)
        ShellExecuteW(hwnd_, L"open", L"notepad.exe",
                      ToolsFilePath().c_str(), nullptr, SW_SHOWNORMAL);
}

// 启动外部工具：工作目录 = 该窗格当前分页的目录；%DIR% 替换为该目录
void MainWindow::RunPaneTool(size_t paneIdx, const std::wstring& name,
                             const std::wstring& cmdline)
{
    if (paneIdx >= panes_.size()) return;
    std::wstring dir = PaneActiveTab(paneIdx).dir;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(hwnd_, L"当前目录不存在，无法打开外部工具。",
                    name.c_str(), MB_OK | MB_ICONWARNING);
        return;
    }

    std::wstring line = cmdline;
    for (size_t pos = 0; (pos = line.find(L"%DIR%", pos)) != std::wstring::npos; ) {
        line.replace(pos, 5, dir);
        pos += dir.size();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi)) {
        std::wstring msg = L"无法启动工具：\n" + line +
                           L"\n（错误码 " + std::to_wstring(GetLastError()) + L"）";
        MessageBoxW(hwnd_, msg.c_str(), name.c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
void MainWindow::Layout()
{
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    int y = 0;

    // 摆子控件时**不要**逐个立即重绘。原来用 MoveWindow(..., TRUE)，拖分隔条时每个
    // 鼠标移动都会让几十个控件各自同步重绘一次，中间态全被看见 -> 整个界面闪动。
    // 现在只挪不画（SWP_NOREDRAW），全部摆好后由末尾一次 RedrawWindow 统一刷新，
    // 和资源管理器的做法一致：只呈现最终状态。
    auto place = [&](HWND h, int x, int yy, int w, int hh) {
        if (h == nullptr) return;
        SetWindowPos(h, nullptr, x, yy, w, hh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    };

    // 工具栏行
    place(btnBack_,   4, y + 4, 40, 26);
    place(btnFwd_,   48, y + 4, 40, 26);
    place(btnUp_,    92, y + 4, 40, 26);
    place(btnRefresh_,136, y + 4, 50, 26);
    int filterButtonX = W - 190;              // 菜单过滤词按钮左缘（要给右侧“设置”腾位）
    if (filterButtonX < 274) filterButtonX = 274;
    int settingsX = W - 82;                  // “设置”按钮固定贴右边
    if (settingsX < filterButtonX + 112) settingsX = filterButtonX + 112;
    place(address_, 194, y + 6, filterButtonX - 198, 24);
    place(btnMenuFilters_, filterButtonX, y + 4, 106, 26);
    place(btnSettings_, settingsX, y + 4, 76, 26);
    y += 36;

    // 状态栏占据底部一条，先量出它的高度，分页栏放在它上面
    RECT rs; SendMessageW(status_, WM_SIZE, 0, MAKELPARAM(W, H));
    GetWindowRect(status_, &rs);
    int statusH = rs.bottom - rs.top;

    // 分页栏关闭时整条隐藏，28px 高度还给文件列表
    int pagerH = paginationEnabled_ ? 28 : 0;
    int pagerY = H - statusH - pagerH;
    int listH = pagerY - y;

    // 左侧 Tab 容器：先放 tab，再把内容页放到 tab 显示区内
    // （目录树与收藏列表是 tab_ 的子窗口，坐标相对 tab_ 客户区）
    int sideW = sideWidth_;
    place(tab_, 0, y, sideW, listH);
    RECT rt{};
    SendMessageW(tab_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rt));
    int tabH = rt.bottom - rt.top;
    int treeTop = tabH + 6;
    int innerH = listH - treeTop - 6;
    int sideContentH = listH - tabH - 12;
    place(btnTreeSync_, sideW - 42 - 2, treeTop + 2, 36, 20);
    place(directoryTree_.Handle(), 4, treeTop, sideW - 8, innerH);
    SetWindowPos(btnTreeSync_, HWND_TOP, sideW - 42, treeTop + 2, 36, 20,
                 SWP_NOACTIVATE);
    place(favList_, 4, tabH + 6, sideW - 8, sideContentH);

    // 右侧多窗格网格布局：
    //   1 个：独占；2 个：左右；3 个：品/倒品（triLayout_）；4 个：田字形
    //   同排两个窗格的宽度按 Pane::width 比例分配（分隔条可拖）
    int gx = sideW + kSplitGap;
    int gw = W - gx;
    int gh = listH;
    const int S = kSplitGap; // 分隔条厚度（视觉留白）
    paneRects_.clear();
    splitPairs_.clear();
    rowSplitY_ = -1;
    rowSplitTop_ = y;
    rowSplitBottom_ = y + gh;

    size_t n = panes_.size();
    std::vector<RECT> rects(n);
    auto rowW = [&](size_t a, size_t b) { // 同排 a|b 两窗格的分割宽度
        int wsum = panes_[a].width + panes_[b].width;
        if (wsum <= 0) wsum = 2;
        int w = (int)((long long)(gw - S) * panes_[a].width / wsum);
        if (w < 160) w = 160;
        if (gw - S - w < 160) w = gw - S - 160;
        if (w < 160) w = 160;
        return w;
    };
    if (n == 1) {
        rects[0] = { gx, y, gx + gw, y + gh };
    } else if (n == 2) {
        int w = rowW(0, 1);
        rects[0] = { gx, y, gx + w, y + gh };
        rects[1] = { gx + w + S, y, gx + gw, y + gh };
        splitPairs_.push_back({ 0, 1 });
    } else if (n == 3) {
        int availableH = gh - S;
        int halfH = (int)((long long)availableH * rowSplitPermille_ / 1000);
        if (halfH < 120) halfH = 120;
        if (availableH - halfH < 120) halfH = availableH - 120;
        rowSplitY_ = y + halfH;
        if (triLayout_ == 1) { // 倒品字形：2 上 1 下（默认）
            int w = rowW(0, 1);
            rects[0] = { gx, y, gx + w, y + halfH };
            rects[1] = { gx + w + S, y, gx + gw, y + halfH };
            rects[2] = { gx, y + halfH + S, gx + gw, y + gh };
            splitPairs_.push_back({ 0, 1 }); // 竖分隔条在上方两窗格之间
        } else {               // 品字形：1 上 2 下
            int w = rowW(1, 2);
            rects[0] = { gx, y, gx + gw, y + halfH };
            rects[1] = { gx, y + halfH + S, gx + w, y + gh };
            rects[2] = { gx + w + S, y + halfH + S, gx + gw, y + gh };
            splitPairs_.push_back({ 1, 2 }); // 竖分隔条在下方两窗格之间
        }
    } else if (n >= 4) { // 4：田字形（n==0 时哪个分支都不走：窗格尚未创建）
        int availableH = gh - S;
        int halfH = (int)((long long)availableH * rowSplitPermille_ / 1000);
        if (halfH < 120) halfH = 120;
        if (availableH - halfH < 120) halfH = availableH - 120;
        rowSplitY_ = y + halfH;
        int wTop = rowW(0, 1);
        int wBot = rowW(2, 3);
        rects[0] = { gx, y, gx + wTop, y + halfH };
        rects[1] = { gx + wTop + S, y, gx + gw, y + halfH };
        rects[2] = { gx, y + halfH + S, gx + wBot, y + gh };
        rects[3] = { gx + wBot + S, y + halfH + S, gx + gw, y + gh };
        splitPairs_.push_back({ 0, 1 });
        splitPairs_.push_back({ 2, 3 });
    }

    RECT rrt{};
    int rtabH = 24;
    for (size_t i = 0; i < panes_.size(); ++i) {
        Pane& p = panes_[i];
        RECT& r = rects[i];
        int w = r.right - r.left, hh = r.bottom - r.top;
        ShowWindow(p.tab, SW_SHOW); // 曾经被隐藏过（切换布局）的窗格要恢复
        place(p.tab, r.left, r.top, w, hh);
        paneRects_.push_back(r);
        SendMessageW(p.tab, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rrt));
        rtabH = rrt.bottom - rrt.top;
        // list 是 p.tab 的子窗口，坐标相对 p.tab 客户区
        place(p.list, 4, rtabH + 6, w - 8, hh - rtabH - 12);
    }

    // 每个窗格的“+”按钮贴在其最后一个分页头右侧（UpdateNewTabButtons 里算）
    UpdateNewTabButtons();

    // 记录分隔条可拖动的水平区间（供命中测试）
    splitTop_ = y;
    splitBot_ = pagerY;

    // 分页栏：关闭分页开关时整条隐藏
    {
        HWND pagerCtls[] = { pagerFirst_, pagerPrev_, pagerLabel_, pagerNext_, pagerLast_, pagerSize_ };
        for (HWND h : pagerCtls)
            if (h) ShowWindow(h, paginationEnabled_ ? SW_SHOWNA : SW_HIDE);
    }
    if (paginationEnabled_) {
        int px = 4;
        place(pagerFirst_, px, pagerY + 2, 40, 24); px += 44;
        place(pagerPrev_,  px, pagerY + 2, 40, 24); px += 44;
        place(pagerLabel_, px, pagerY + 4, 160, 20); px += 164;
        place(pagerNext_,  px, pagerY + 2, 40, 24); px += 44;
        place(pagerLast_,  px, pagerY + 2, 40, 24); px += 48;
        place(pagerSize_,  px, pagerY + 2, 100, 24);
    }

    // 状态栏自动布局
    SendMessageW(status_, WM_SIZE, 0, MAKELPARAM(W, H));

    // 所有子控件都已就位，统一重绘一次（只呈现最终状态，避免中间态闪动）
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

LRESULT MainWindow::ListViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN || msg == WM_SETCURSOR) {
        POINT pt{};
        if (msg == WM_LBUTTONDOWN) {
            pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
        } else {
            GetCursorPos(&pt);
        }
        ScreenToClient(hwnd_, &pt);

        if (rowSplitY_ >= 0 && !paneRects_.empty() &&
            pt.y >= rowSplitY_ - kSplitHit && pt.y <= rowSplitY_ + kSplitHit) {
            int left = paneRects_.front().left;
            int right = paneRects_.front().right;
            for (const RECT& r : paneRects_) {
                if (r.left < left) left = r.left;
                if (r.right > right) right = r.right;
            }
            if (pt.x >= left && pt.x <= right) {
                if (msg == WM_LBUTTONDOWN) {
                    SendMessageW(hwnd_, WM_LBUTTONDOWN, wp, MAKELPARAM(pt.x, pt.y));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                }
                return msg == WM_SETCURSOR ? TRUE : 0;
            }
        }
    }

    // 慢双击（Explorer 习惯）：先单击选中、停顿一下再单击同一项 -> 进入重命名。
    // 快速双击会被系统判定为双击、走 NM_DBLCLK 打开/进入；这里只拦截“慢”的那一次点击。
    if (msg == WM_LBUTTONDOWN) {
        LVHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = ListView_HitTest(h, &ht);
        DWORD now = GetMessageTime();
        DWORD delta = (DWORD)(now - lastRenameClickTime_);
        bool modifier = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000);
        if (modifier) {
            // Ctrl/Shift 是选择修饰键，不参与慢双击，并清掉记录避免误触发
            lastRenameList_ = nullptr;
            lastRenameClickItem_ = -1;
            lastRenameClickTime_ = 0;
        } else if (idx >= 0 && h == lastRenameList_ && idx == lastRenameClickItem_ &&
                   ListView_GetItemState(h, idx, LVIS_SELECTED) &&
                   delta >= (DWORD)GetDoubleClickTime() && delta <= kSlowRenameWindow) {
            lastRenameList_ = nullptr;    // 复位，避免连点连续触发
            lastRenameClickItem_ = -1;
            lastRenameClickTime_ = 0;
            int rpi = PaneOfList(h);
            if (rpi >= 0) {
                std::wstring path = PaneItemPath((size_t)rpi, idx);
                if (!path.empty()) {
                    if ((size_t)rpi != activePane_) SelectPane((size_t)rpi);
                    RenamePath(path);
                }
            }
            return 0;   // 吃掉这次点击（项已选中，无需再改选择）
        } else {
            lastRenameList_ = h;
            lastRenameClickItem_ = idx;   // 空白处 idx<0 也记录，会覆盖上一项
            lastRenameClickTime_ = now;
        }
    }

    int pi = PaneOfList(h);
    if (pi >= 0 && msg == WM_KEYDOWN && wp == VK_F2) {
        int selected = ListView_GetNextItem(h, -1, LVNI_SELECTED);
        std::wstring path = PaneItemPath(static_cast<size_t>(pi), selected);
        if (!path.empty()) {
            if (static_cast<size_t>(pi) != activePane_)
                SelectPane(static_cast<size_t>(pi));
            RenamePath(path);
        }
        return 0;
    }
    // 只有真正的用户输入（点击/按键）才激活窗格并同步目录树；
    // 鼠标移动、悬停重绘、tooltip 等带来的消息一律不切换。
    if (pi >= 0 && (size_t)pi != activePane_ &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
         msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))
        SelectPane((size_t)pi);
    // 文件视图的常规键盘操作（资源管理器习惯）：Ctrl+C 复制 / Ctrl+X 剪切 /
    // Ctrl+V 粘贴 / Delete 删除到回收站 / Shift+Delete 直接删除。
    if (pi >= 0 && msg == WM_KEYDOWN) {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wp == VK_F2)
            WriteAppLog((L"RENAME key message received by list: pane=" +
                         std::to_wstring(pi)).c_str());
        if (ctrl && wp == 'C')      { OnClipboard(false); return 0; }
        if (ctrl && wp == 'X')      { OnClipboard(true);  return 0; }
        if (ctrl && wp == 'V')      { OnPaste();          return 0; }
        if (wp == VK_DELETE)        { OnDelete(!shift);   return 0; }
    }
    if (pi >= 0 && msg == WM_CONTEXTMENU) {
        // 右键菜单：在列表空白/条目上弹出 Explorer 风格菜单
        if ((size_t)pi != activePane_) SelectPane((size_t)pi); // 先激活该窗格
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        bool keyboardContext = pt.x == -1 && pt.y == -1;
        if (keyboardContext) { // 键盘触发
            pt = { 200, 200 };
            ClientToScreen(hwnd_, &pt);
        }
        std::wstring path;
        if (keyboardContext) {
            int sel = ListView_GetNextItem(h, -1, LVNI_SELECTED);
            if (sel >= 0) path = PaneItemPath((size_t)pi, sel);
        } else {
            POINT clientPt = pt;
            ScreenToClient(h, &clientPt);
            LVHITTESTINFO hit{};
            hit.pt = clientPt;
            int item = ListView_HitTest(h, &hit);
            if (item >= 0) {
                path = PaneItemPath((size_t)pi, item);
            } else {
                // 空白处使用文件夹背景菜单，而不是沿用之前残留的选中项。
                ListView_SetItemState(h, -1, 0, LVIS_SELECTED);
            }
        }
        bool addFavorite = false;
        std::wstring createdFolderPath;
        bool renameSelected = false;
        if (shell::ShowContextMenu(hwnd_, path, PaneActiveTab((size_t)pi).dir, pt,
                                  L"添加当前目录到收藏", addFavorite, &createdFolderPath,
                                  &renameSelected)) {
            WriteAppLog((L"CONTEXT_MENU command completed: pane=" +
                         std::to_wstring(pi) + L", selectedPath=" +
                         (path.empty() ? L"(background)" : path) +
                         L", createdFolder=" +
                         (createdFolderPath.empty() ? L"(none)" : createdFolderPath)).c_str());
            if (!createdFolderPath.empty())
                RenamePath(createdFolderPath);
            RefreshListFromDisk();   // 右键菜单可能增删改了文件（删除/粘贴/重命名）
        } else {
            WriteAppLog((L"CONTEXT_MENU command not invoked: pane=" +
                         std::to_wstring(pi) + L", selectedPath=" +
                         (path.empty() ? L"(background)" : path)).c_str());
        }
        if (renameSelected && !path.empty()) {
            WriteAppLog((L"CONTEXT_MENU rename selected: " + path).c_str());
            RenamePath(path);
        }
        if (addFavorite) OnAddFavorite();
        return 0;
    }
    if (pi >= 0 && msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        // 列表聚焦时 ESC 同样等同于点关闭按钮：收进托盘
        HideToTray();
        return 0;
    }
    // 每个窗格各自保存原过程，别用别窗格的
    WNDPROC orig = (pi >= 0) ? panes_[pi].listOld : nullptr;
    if (orig) return CallWindowProcW(orig, h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::ListViewProcStatic(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                                UINT_PTR, DWORD_PTR ref)
{
    // 用 GWLP_USERDATA 取 self（CreateListView 时已设置）
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->ListViewProc(h, m, wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == GetTaskbarBroadcastMessage()) {
        WriteAppLog(L"TASKBAR_CREATED notification received");
        if (!trayIcon_.RestoreAfterTaskbarRestart()) {
            WriteAppLog(L"TASKBAR_CREATED tray icon restore failed");
            MessageBoxW(hwnd_, L"系统托盘恢复后无法重新创建 PagedExplorer 图标。",
                        L"PagedExplorer", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    switch (msg) {
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) {
            WriteAppLog(L"WM_SYSCOMMAND SC_CLOSE received");
            HideToTray();
            return 0;
        }
        break;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;

    // 用户拖动/缩放结束后落盘一次，保证窗口尺寸与位置在进程重启后被恢复
    case WM_EXITSIZEMOVE:
        WriteAppLog(L"WM_EXITSIZEMOVE: saving session (window move/resize ended)");
        SaveSession();
        return 0;

    // 关机 / 注销：这是 WM_DESTROY 之前最后的落盘机会。少了它，系统结束进程时
    // 分页锁定状态、窗格布局等都保不住（点 X 收托盘、托盘退出都会存，只有这条路不会）。
    case WM_QUERYENDSESSION:
        WriteAppLog(L"WM_QUERYENDSESSION: saving session before shutdown/logoff");
        SaveSession();
        return TRUE;   // 允许结束

    case WM_ENDSESSION:
        // wp 为 FALSE 表示结束被否决了；但真正结束前也再兜一次底
        if (wp) SaveSession();
        return 0;

    case WM_WINDOWPOSCHANGED: {
        // Aero Snap（Win+←/→/↑）等由系统触发的尺寸/位置变化不会触发 WM_EXITSIZEMOVE，
        // 必须在窗口实际位置改变时落盘，否则重启后无法恢复 Snap 后的半屏尺寸/位置。
        // 限流 300ms 并在矩形无变化时跳过，避免拖动过程中频繁写盘。
        if (!restoreInProgress_) {
            RECT now{};
            if (GetWindowRect(hwnd_, &now) &&
                (now.left != lastSavedWinRect_.left || now.top != lastSavedWinRect_.top ||
                 now.right != lastSavedWinRect_.right || now.bottom != lastSavedWinRect_.bottom)) {
                DWORD t = GetTickCount();
                if (t - lastSaveTick_ > 300) {
                    lastSaveTick_ = t;
                    lastSavedWinRect_ = now;
                    SaveSession();
                }
            }
        }
        break;
    }

    case WM_CLOSE:
        WriteAppLog(L"WM_CLOSE received");
        HideToTray();
        return 0;

    case TrayIcon::CallbackMessage:
        switch (TrayAction action = trayIcon_.HandleCallback(lp)) {
        case TrayAction::Open:
            WriteAppLog(L"TRAY_ACTION open");
            ShowFromTray();
            break;
        case TrayAction::Settings:
            WriteAppLog(L"TRAY_ACTION settings");
            OpenSettings();
            break;
        case TrayAction::Exit:
            WriteAppLog(L"TRAY_ACTION exit");
            DestroyWindow(hwnd_);
            break;
        case TrayAction::None:
            break;
        }
        return 0;

    case WM_KEYDOWN: // Ctrl+方向键：切换右侧分页
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == VK_LEFT) {
            if (activeTab_ > 0) SelectRightTab(activeTab_ - 1);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == VK_RIGHT) {
            if (activeTab_ + 1 < tabs_.size()) SelectRightTab(activeTab_ + 1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            // 与标题栏“关闭”按钮（X）一致：收进托盘。
            // 仅在非文本编辑态拦截——编辑框/组合框聚焦时的 ESC 交给控件自身处理，
            // 避免正在输入路径时误关窗口。
            HWND f = GetFocus();
            if (f == hwnd_ || f == nullptr) { HideToTray(); return 0; }
            wchar_t cls[32] = { 0 };
            GetClassNameW(f, cls, 31);
            if (_wcsicmp(cls, L"Edit") != 0 && _wcsicmp(cls, L"ComboBox") != 0) {
                HideToTray();
                return 0;
            }
        }
        break;

    case WM_SYSKEYDOWN: // Alt+数字：窗格数量/布局；Alt+方向键：导航
        // Alt+1~4 切换窗格数量（主键盘数字行；小键盘 2 另有“倒品字形”用途，不参与）
        if (wp >= '1' && wp <= '4') {
            SetPaneCount((int)(wp - '0'));
            return 0;
        }
        // Alt+小键盘8：品字形(1上2下)；Alt+小键盘2：倒品字形(2上1下)
        // （NumLock 关闭时小键盘 8/2 上报为 VK_UP/VK_DOWN，这里按非扩展键处理）
        if (wp == VK_NUMPAD8) { SetTriLayout(0); return 0; }
        if (wp == VK_NUMPAD2 || wp == VK_DOWN) { SetTriLayout(1); return 0; }
        switch (wp) {
        case VK_UP: {
            const std::wstring& dir = CurTab().dir;
            if (dir.size() > 3) {
                size_t p = dir.find_last_of(L'\\');
                if (p != std::wstring::npos && p > 2) Navigate(dir.substr(0, p));
            }
            return 0;
        }
        case VK_LEFT: {
            TabState& t = CurTab();
            if (t.histPos > 0) { --t.histPos; Navigate(t.history[t.histPos], false); }
            return 0;
        }
        case VK_RIGHT: {
            TabState& t = CurTab();
            if (t.histPos + 1 < (int)t.history.size()) { ++t.histPos; Navigate(t.history[t.histPos], false); }
            return 0;
        }
        }
        break;

    case WM_SYSCHAR:
        return 0;   // 窗口无菜单：吞掉 Alt 组合键的默认处理（避免提示音）

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        // 多窗格：窗格被删除后控件 ID 与下标不再对应，统一用 hwnd 反查窗格
        int pi = PaneOfList(nm->hwndFrom);
        if (pi >= 0) {
            // 操作非激活窗格 -> 先激活它。只认真正的用户交互通知（点击/键盘/点列头）：
            // 鼠标移动、悬停引起的重绘（NM_CUSTOMDRAW）、tooltip 等通知一律不切换窗格。
            switch (nm->code) {
            case NM_CLICK:
            case NM_DBLCLK:
            case NM_RCLICK:
            case NM_RDBLCLK:
            case NM_RETURN:
            case LVN_COLUMNCLICK:
            case LVN_KEYDOWN:
                if ((size_t)pi != activePane_) SelectPane((size_t)pi);
                break;
            }
            if (nm->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
                TabState& vt = PaneActiveTab((size_t)pi); // 取本窗格自己的分页数据
                int i = di->item.iItem;
                if (i < 0 || i >= (int)vt.pageItems.size()) return 0;
                FileEntry& e = vt.pageItems[i];
                if (di->item.mask & LVIF_TEXT) {
                    std::wstring text;
                    switch (di->item.iSubItem) {
                    case COL_NAME: text = e.name; break;
                    case COL_TYPE: text = e.isFolder ? L"文件夹" : shell::TypeNameForEntry(e.path, false); break;
                    case COL_SIZE: text = e.isFolder ? L"" : FormatSize(e.size); break;
                    case COL_MTIME: text = FormatTime(e.writeTime); break;
                    }
                    wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
                }
                if (di->item.mask & LVIF_IMAGE)
                    di->item.iImage = EnsureIcon(e);
            }
            else if (nm->code == NM_DBLCLK) {
                auto* ni = reinterpret_cast<NMITEMACTIVATE*>(nm);
                if (ni->iItem >= 0) {
                    std::wstring p = CurrentPagePath(ni->iItem);
                    if (!p.empty()) {
                        DWORD attr = GetFileAttributesW(p.c_str());
                        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                            Navigate(p);
                        else {
                            // 工作目录 = 当前文件视图所在目录，BAT/可执行文件
                            // 内的相对路径以该目录为基准。
                            SHELLEXECUTEINFOW sei{};
                            sei.cbSize = sizeof(sei);
                            sei.hwnd = hwnd_;
                            sei.lpVerb = L"open";
                            sei.lpFile = p.c_str();
                            sei.lpDirectory = PaneActiveTab((size_t)pi).dir.c_str();
                            sei.nShow = SW_SHOWNORMAL;
                            ShellExecuteExW(&sei);
                        }
                    }
                }
            }
            else if (nm->code == LVN_COLUMNCLICK) {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
                OnColumnClick(lv->iSubItem);
            }
            else if (nm->code == LVN_KEYDOWN) {
                auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
                if (kd->wVKey == VK_F2) {
                    WriteAppLog((L"RENAME LVN_KEYDOWN received: pane=" +
                                 std::to_wstring(pi)).c_str());
                    OnRename();
                }
                // Delete/Shift+Delete 在 ListViewProc 的 WM_KEYDOWN 里处理
            }
            else if (nm->code == LVN_ODFINDITEM) {
                // 虚拟列表的类型前置搜索：键入字符时控件要求我们给出匹配项下标，
                // 不处理的话输入 g 无法定位到 github 这类条目
                auto* fi = reinterpret_cast<NMLVFINDITEM*>(nm);
                TabState& t = PaneActiveTab((size_t)pi);
                int n = (int)t.pageItems.size();
                const wchar_t* prefix = fi->lvfi.psz;
                if (n == 0 || !prefix || !*prefix) return -1;
                int start = fi->iStart;
                if (start < 0 || start >= n) start = 0;
                size_t plen = wcslen(prefix);
                for (int k = 0; k < n; ++k) {           // 从 iStart 起环形查找
                    int i = (start + k) % n;
                    const std::wstring& name = t.pageItems[i].name;
                    if (name.size() >= plen &&
                        _wcsnicmp(name.c_str(), prefix, plen) == 0) {
                        // 自己完成选中+滚动：不能只依赖返回值（实测控件对转发
                        // 过来的通知结果处理并不可靠）
                        HWND lv = nm->hwndFrom;
                        ListView_SetItemState(lv, i,
                                              LVIS_SELECTED | LVIS_FOCUSED,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(lv, i, FALSE);
                        return i;                        // 返回匹配下标（不是 0！）
                    }
                }
                return -1;
            }
        }
        else if (nm->code == TCN_SELCHANGE && PaneOfTab(nm->hwndFrom) >= 0) {
            // 某个窗格的 tab 头点击：切换该窗格的当前分页
            int tipi = PaneOfTab(nm->hwndFrom);
            size_t inPane = (size_t)SendMessageW(panes_[tipi].tab, TCM_GETCURSEL, 0, 0);
            if (inPane < panes_[tipi].tabs.size()) {
                panes_[tipi].active = inPane;
                SelectRightTab(panes_[tipi].tabs[inPane]);
            }
        }
        else if (nm->hwndFrom == tab_ && nm->code == TCN_SELCHANGE) {
            SwitchSideTab((int)SendMessageW(tab_, TCM_GETCURSEL, 0, 0));
        }
        else if (nm->idFrom == IDC_FAVLIST) {
            if (nm->code == NM_CLICK || nm->code == NM_DBLCLK) { // 单击即导航
                auto* ni = reinterpret_cast<NMITEMACTIVATE*>(nm);
                if (ni->iItem >= 0) {
                    LVITEMW it{}; it.iItem = ni->iItem; it.mask = LVIF_PARAM;
                    ListView_GetItem(favList_, &it);
                    auto* dir = reinterpret_cast<std::wstring*>(it.lParam);
                    if (dir && !dir->empty()) NavigateFromSidebar(*dir);
                }
            }
            else if (nm->code == NM_RCLICK) {
                auto* ni = reinterpret_cast<NMITEMACTIVATE*>(nm);
                POINT pt = ni->ptAction;
                ClientToScreen(favList_, &pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"打开");
                AppendMenuW(menu, MF_STRING, 2, L"删除收藏");
                int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                           pt.x, pt.y, hwnd_, nullptr);
                DestroyMenu(menu);
                if (cmd == 1) {
                    LVITEMW it{}; it.iItem = ni->iItem; it.mask = LVIF_PARAM;
                    ListView_GetItem(favList_, &it);
                    auto* dir = reinterpret_cast<std::wstring*>(it.lParam);
                    if (dir && !dir->empty()) NavigateFromSidebar(*dir);
                } else if (cmd == 2) {
                    OnRemoveFavorite();
                }
            }
        }
        else {
            std::wstring selectedTreePath;
            if (directoryTree_.HandleNotification(nm, selectedTreePath)) {
                if (!selectedTreePath.empty()) NavigateFromSidebar(selectedTreePath);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_BACK: {
            TabState& t = CurTab();
            if (t.histPos > 0) { --t.histPos; Navigate(t.history[t.histPos], false); }
            return 0;
        }
        case IDC_FORWARD: {
            TabState& t = CurTab();
            if (t.histPos + 1 < (int)t.history.size()) { ++t.histPos; Navigate(t.history[t.histPos], false); }
            return 0;
        }
        case IDC_UP: {
            // 上一级：逐级去掉最后一段。一级文件夹的上一级就是盘根（F:\tts_out -> F:\），
            // 原先的 find_last_of + (p>2) 判断会把这种“父目录就是盘根”的情况误拦，
            // 所以这里显式区分：父目录只剩盘符时补上反斜杠。
            const std::wstring& dir = CurTab().dir;
            size_t end = dir.size();
            while (end > 0 && (dir[end - 1] == L'\\' || dir[end - 1] == L'/')) --end; // 去尾分隔符
            if (end <= 2) return 0;                    // 已在盘根（如 F:\）或无效路径，不动
            size_t p = dir.find_last_of(L"\\/", end - 1);
            std::wstring parent = (p == std::wstring::npos)
                ? dir.substr(0, 2) + L'\\'             // "F:sub" 盘相对路径 -> 盘根
                : dir.substr(0, p);
            if (parent.size() == 2) parent += L'\\';   // "F:" -> "F:\"
            if (!parent.empty() && parent != dir) Navigate(parent);
            return 0;
        }
        default:
            // 每个窗格一个“+”按钮，命令 ID 落在 IDC_NEWTAB_BASE..+7 区间
            if (id >= IDC_NEWTAB_BASE && id < IDC_NEWTAB_BASE + 8) {
                int tag = id - IDC_NEWTAB_BASE;
                int paneIdx = -1;
                for (size_t i = 0; i < panes_.size(); ++i)
                    if (panes_[i].tag == tag) { paneIdx = (int)i; break; }
                if (paneIdx >= 0) {
                    if ((GetKeyState(VK_CONTROL) & 0x8000) && panes_.size() < 4)
                        AddPane();                        // Ctrl+点击：新增窗格（最多 4 个）
                    else
                        AddRightTab(true, (size_t)paneIdx); // 普通点击：在该窗格加一个分页
                }
                return 0;
            }
            // 每个窗格一个“▾”外部工具按钮
            if (id >= IDC_TOOLS_BASE && id < IDC_TOOLS_BASE + 8) {
                int tag = id - IDC_TOOLS_BASE;
                for (size_t i = 0; i < panes_.size(); ++i)
                    if (panes_[i].tag == tag) { ShowPaneToolsMenu(i); break; }
                return 0;
            }
            break;
        case IDC_TREE_SYNC:
            SyncTreeToCurrentTab();
            return 0;
        case IDC_TM_CLOSE:  CloseRightTab(menuTab_); return 0;
        case IDC_TM_OTHERS: CloseOtherTabs(menuTab_); return 0;
        case IDC_TM_RIGHT:  CloseRightTabs(menuTab_); return 0;
        case IDC_TM_LOCK:   ToggleTabLock(menuTab_); return 0;
        case IDC_LAYOUT1: SetPaneCount(1); return 0;
        case IDC_LAYOUT2: SetPaneCount(2); return 0;
        case IDC_LAYOUT3: SetPaneCount(3); return 0;
        case IDC_LAYOUT4: SetPaneCount(4); return 0;
        case IDC_TRI_PINTOP:  SetTriLayout(0); return 0; // 品字形：1 上 2 下
        case IDC_TRI_PINDOWN: SetTriLayout(1); return 0; // 倒品字形：2 上 1 下
        case IDC_REFRESH: RefreshListFromDisk(); return 0;   // 刷新=真正重新枚举目录
        case IDC_MENU_FILTERS:
            shell::OpenContextMenuFilterSettings(hwnd_);
            return 0;
        case IDC_SETTINGS:
            OpenSettings();
            return 0;
        case IDC_FIRST: CurTab().curPage = 0; RefreshList(); return 0;
        case IDC_PREV:  if (CurTab().curPage > 0) { --CurTab().curPage; RefreshList(); } return 0;
        case IDC_NEXT:
            if (CurTab().curPage + 1 < CurTab().pages->PageCount()) { ++CurTab().curPage; RefreshList(); }
            return 0;
        case IDC_LAST:
            if (CurTab().pages->PageCount() > 0) { CurTab().curPage = CurTab().pages->PageCount() - 1; RefreshList(); }
            return 0;
        case IDC_PAGE_SIZE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(pagerSize_, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < kPageSizeCount) pageSize_ = (size_t)kPageSizes[sel];
                CurTab().curPage = 0;
                Navigate(CurTab().dir, false);
            }
            return 0;
        case IDC_ADDRESS:
            if (HIWORD(wp) == EN_KILLFOCUS || HIWORD(wp) == EN_ADDR_RETURN) {
                wchar_t buf[MAX_PATH * 2] = {};
                GetWindowTextW(address_, buf, MAX_PATH * 2);
                if (buf[0] && buf != CurTab().dir) {
                    // 锁定只挡关闭与地址变化：激活的是锁定分页时，改走侧栏同款逻辑，
                    // 在同一窗格里新开一个分页打开新地址，不动锁定分页本身
                    if (CurTab().locked) NavigateFromSidebar(buf);
                    else Navigate(buf);
                }
                if (HIWORD(wp) == EN_ADDR_RETURN) SetFocus(CurList());
            }
            return 0;
        }
        return 0;
    }

    case WM_APP_CLOSE_TAB:      // 中键点击分页标题（延后到这里真正关闭）
        CloseRightTab(pendingCloseTab_);
        pendingCloseTab_ = SIZE_MAX;
        return 0;

    case WM_APP_MOVE_TAB:       // 拖拽分页到别的窗格（延后到这里真正移动）
        if (pendingMoveTab_ != SIZE_MAX && pendingMovePane_ >= 0 &&
            pendingMoveTab_ < tabs_.size() && (size_t)pendingMovePane_ < panes_.size())
            MoveTabToPane(pendingMoveTab_, (size_t)pendingMovePane_);
        pendingMoveTab_ = SIZE_MAX;
        pendingMovePane_ = -1;
        return 0;

    case WM_APP_ADD_TAB:        // 双击分页栏空白（延后到这里真正新建，见 PaneTabHandler）
        if (static_cast<size_t>(wp) < panes_.size())
            AddRightTab(true, static_cast<size_t>(wp));
        return 0;

    case WM_APP_TREE_NEWTAB: {  // 目录树中键（延后到这里真正新建，见 CreateSidePanel）
        auto* path = reinterpret_cast<std::wstring*>(wp);
        if (path) {
            OpenDirInNewTab(*path);
            delete path;
        }
        return 0;
    }

    case WM_APP_PAGELOADED:
        OnPageLoaded();
        return 0;

    case WM_COPYDATA: { // 二次实例转发来的路径
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (cds && cds->lpData && cds->cbData > 0) {
            // 发送方 cbData 含结尾 '\0'；按 C 字符串构造，别把 '\0' 带进字符串，
            // 否则 dir 末尾的嵌入 '\0' 会让枚举 pattern 变成目录本身（列表只剩目录一项）
            std::wstring path(static_cast<const wchar_t*>(cds->lpData));
            OpenTarget(path);
            ShowFromTray();
        }
        return TRUE;
    }

    // Win+E 被全局钩子拦截后，激活本窗口（还原最小化 / 置前）
    case WM_APP_WIN_E: {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        else if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
        // SetForegroundWindow 只有在前台进程才有权限；Win+E 时前台属于
        // 别的进程，直接调用只会闪任务栏。经典解法：AttachThreadInput
        // 短暂挂到前台线程，借用其输入权限完成置前。
        HWND fg = GetForegroundWindow();
        DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD myTid = GetCurrentThreadId();
        bool attached = fgTid && fgTid != myTid &&
                        AttachThreadInput(myTid, fgTid, TRUE);
        BringWindowToTop(hwnd_);
        SetForegroundWindow(hwnd_);
        if (attached) AttachThreadInput(myTid, fgTid, FALSE);
        if (activePane_ < panes_.size()) SetFocus(CurList());
        return 0;
    }

    // 分隔条拖动：sideW .. sideW+12 之间是空命中区，鼠标落在此处归主窗口
    case WM_LBUTTONDOWN: {
        int x = static_cast<int>(GET_X_LPARAM(lp));
        int yy = static_cast<int>(GET_Y_LPARAM(lp));
        if (rowSplitY_ >= 0 && !paneRects_.empty() &&
            yy >= rowSplitY_ - kSplitHit && yy <= rowSplitY_ + kSplitHit) {
            int left = paneRects_.front().left;
            int right = paneRects_.front().right;
            for (const RECT& r : paneRects_) {
                if (r.left < left) left = r.left;
                if (r.right > right) right = r.right;
            }
            if (x >= left && x <= right) {
                rowSplitDragging_ = true;
                SetCapture(hwnd_);
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                return 0;
            }
        }
        // 窗格间分隔条命中：splitPairs_ 各对的右缘 .. +12（限该排高度内）
        if (yy >= splitTop_ && yy <= splitBot_) {
            for (size_t k = 0; k < splitPairs_.size(); ++k) {
                auto& pr = splitPairs_[k];
                int edge = paneRects_[pr.first].right;
                int top = paneRects_[pr.first].top, bot = paneRects_[pr.first].bottom;
                if (x >= edge && x <= edge + kSplitHit && yy >= top && yy <= bot) {
                    paneSplitDragging_ = true;
                    paneSplitIndex_ = (int)k;
                    SetCapture(hwnd_);
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return 0;
                }
            }
        }
        if (x >= sideWidth_ && x <= sideWidth_ + kSplitHit && yy >= splitTop_ && yy <= splitBot_) {
            draggingSplitter_ = true;
            SetCapture(hwnd_);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (rowSplitDragging_) {
            int availableH = rowSplitBottom_ - rowSplitTop_ - kSplitGap;
            if (availableH >= 240) {
                int topH = static_cast<int>(GET_Y_LPARAM(lp)) - rowSplitTop_;
                if (topH < 120) topH = 120;
                if (topH > availableH - 120) topH = availableH - 120;
                int ratio = (int)((long long)topH * 1000 / availableH);
                if (ratio != rowSplitPermille_) {
                    rowSplitPermille_ = ratio;
                    Layout();
                }
            }
            return 0;
        }
        if (paneSplitDragging_) {
            // 拖 splitPairs_[k] 分隔条：调整左右两窗格的 width
            int x = static_cast<int>(GET_X_LPARAM(lp));
            int k = paneSplitIndex_;
            if (k >= 0 && k < (int)splitPairs_.size()) {
                auto& pr = splitPairs_[k];
                size_t a = pr.first, b = pr.second;
                int leftEdge = paneRects_[a].left;
                int want = x - leftEdge;
                int minW = 160;
                int availableW = paneRects_[b].right - leftEdge - kSplitGap;
                int maxW = availableW - minW;
                if (want < minW) want = minW;
                if (want > maxW) want = maxW;
                // Store the actual pixel split as weights so the rendered divider tracks the pointer.
                if (want != paneRects_[a].right - leftEdge) {
                    panes_[a].width = want;
                    panes_[b].width = availableW - want;
                    Layout();
                }
            }
            return 0;
        }
        if (draggingSplitter_) {
            int x = static_cast<int>(GET_X_LPARAM(lp));
            RECT rc; GetClientRect(hwnd_, &rc);
            int maxW = rc.right - 260;                 // 右侧至少留 260px
            int nw = (x < 150) ? 150 : (x > maxW ? maxW : x);
            if (nw != sideWidth_) { sideWidth_ = nw; Layout(); }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (rowSplitDragging_) {
            rowSplitDragging_ = false;
            ReleaseCapture();
            return 0;
        }
        if (paneSplitDragging_) {
            paneSplitDragging_ = false;
            paneSplitIndex_ = -1;
            ReleaseCapture();
            return 0;
        }
        if (draggingSplitter_) {
            draggingSplitter_ = false;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd_, &pt);
        if (rowSplitY_ >= 0 && !paneRects_.empty() &&
            pt.y >= rowSplitY_ - kSplitHit && pt.y <= rowSplitY_ + kSplitHit) {
            int left = paneRects_.front().left;
            int right = paneRects_.front().right;
            for (const RECT& r : paneRects_) {
                if (r.left < left) left = r.left;
                if (r.right > right) right = r.right;
            }
            if (pt.x >= left && pt.x <= right) {
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        // 窗格间分隔条光标
        if (pt.y >= splitTop_ && pt.y <= splitBot_) {
            for (size_t k = 0; k < splitPairs_.size(); ++k) {
                auto& pr = splitPairs_[k];
                int edge = paneRects_[pr.first].right;
                if (pt.x >= edge && pt.x <= edge + kSplitHit &&
                    pt.y >= paneRects_[pr.first].top && pt.y <= paneRects_[pr.first].bottom) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
        }
        if (pt.x >= sideWidth_ && pt.x <= sideWidth_ + kSplitHit &&
            pt.y >= splitTop_ && pt.y <= splitBot_) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;  // 其它区域交给 DefWindowProc 设置默认光标
    }

    case WM_DESTROY:
        WriteAppLog(L"WM_DESTROY received; application window is exiting");
        trayIcon_.Remove();
        for (const Pane& pane : panes_)
            if (pane.list) RevokeDragDrop(pane.list);
        for (auto& t : tabs_) t.pages->Shutdown();
        SaveSession();   // 记住这次打开的所有窗格/分页/历史，下次启动恢复
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::WndProcStatic(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
        return DefWindowProcW(h, m, wp, lp);
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->WndProc(m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}
