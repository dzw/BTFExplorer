#include "MainWindow.h"
#include "../Shell/ShellUtil.h"
#include "../Shell/ShellContextMenu.h"
#include "../Shell/ShellFileOperation.h"
#include <windowsx.h>
#include <shellapi.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

static constexpr int WM_APP_PAGELOADED = WM_APP + 1;
static constexpr UINT WM_APP_WIN_E = WM_APP + 2;   // Win+E 被拦截后激活本窗口
static constexpr int WM_APP_CLOSE_TAB = WM_APP + 3; // 中键点击分页 -> 延后到主窗口关闭
static constexpr int WM_APP_MOVE_TAB  = WM_APP + 4; // 拖拽分页 -> 延后到主窗口移动

// 自定义通知值：Edit 没有 EN_RETURN 常量，回车通知用这个
static constexpr UINT EN_ADDR_RETURN = 0x1000;

static LRESULT CALLBACK AddressProc(HWND h, UINT m, WPARAM wp, LPARAM lp); // 前向声明
static LRESULT CALLBACK PaneTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp); // 分页拖拽 tab 子类化

// ListView 列
enum { COL_NAME = 0, COL_TYPE, COL_SIZE, COL_MTIME };

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
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
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
    self->ApplySavedLayout();   // 窗格数量 / 品字形态：默认上次的布局
    self->PopulateDrives();
    self->Navigate(L"C:\\Users\\Public", false);
    ShowWindow(self->hwnd_, SW_SHOW);
    UpdateWindow(self->hwnd_);
    return self;
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
    btnNewTab_ = CreateWindowExW(0, WC_BUTTONW, L"+",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 26, 22, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEWTAB)), hInst, nullptr);
    if (uiFont_) SendMessageW(btnNewTab_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
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
    for (int n : {50, 100, 200, 500}) {
        wchar_t buf[32]; wsprintfW(buf, L"%d / 页", n);
        SendMessageW(pagerSize_, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(pagerSize_, CB_SETCURSEL, 1, 0); // 100

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
    CreateTree();
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
    ShowWindow(tree_, index == 0 ? SW_SHOW : SW_HIDE);
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

void MainWindow::CreateTree()
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, tab_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE)), hInst, nullptr);
    if (uiFont_) SendMessageW(tree_, WM_SETFONT, (WPARAM)uiFont_, TRUE);

    SHFILEINFOW fi{};
    HIMAGELIST sys = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    if (sys)
        TreeView_SetImageList(tree_, sys, TVSIL_NORMAL);
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
    ListView_SetExtendedListViewStyle(p.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    BuildImageList(p.list);
    InsertColumns(p.list);

    p.listOld = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(p.list, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&MainWindow::ListViewProcStatic)));
    SetWindowLongPtrW(p.list, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
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
    DestroyWindow(panes_[idx].tab);
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

void MainWindow::SelectRightTab(size_t index)
{
    if (index >= tabs_.size()) return;
    TabState& t = tabs_[index];
    activeTab_ = index;
    activePane_ = t.pane;
    Pane& p = panes_[activePane_];
    for (size_t k = 0; k < p.tabs.size(); ++k)
        if (p.tabs[k] == index) { p.active = k; break; }
    SendMessageW(p.tab, TCM_SETCURSEL, p.active, 0);
    SetWindowTextW(address_, t.dir.c_str());
    RefreshList();
}

void MainWindow::SelectPane(size_t index)
{
    if (index >= panes_.size()) return;
    Pane& p = panes_[index];
    if (p.tabs.empty()) return;
    activePane_ = index;
    activeTab_ = p.tabs[p.active];
    SetWindowTextW(address_, CurTab().dir.c_str());
    RefreshList();
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
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(name.c_str());
            SendMessageW(p.tab, TCM_SETITEMW, k, reinterpret_cast<LPARAM>(&ti));
        }
    }
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
    TrackPopupMenuEx(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
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

    t.pages->OpenDirectory(path, pageSize_);
    ListView_SetItemCountEx(CurList(), 0, 0);
    UpdateStatusBar();
    UpdatePaginationBar();
    UpdateRightTabLabels();
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
    if (total > 0)
        wsprintfW(buf, L"共 %I64u 项   第 %I64u / %zu 页   每页 %zu 项",
            total, static_cast<unsigned long long>(CurTab().curPage + 1), CurTab().pages->PageCount(), pageSize_);
    else
        wsprintfW(buf, L"正在统计 %s ...", CurTab().dir.c_str());
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(buf));
}

void MainWindow::UpdatePaginationBar()
{
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

void MainWindow::PopulateDrives()
{
    wchar_t drives[512];
    DWORD n = GetLogicalDriveStringsW(511, drives);
    if (!n) return;
    HIMAGELIST il = TreeView_GetImageList(tree_, TVSIL_NORMAL);

    auto addNode = [&](HTREEITEM parent, const std::wstring& name, const std::wstring& path) -> HTREEITEM {
        TVINSERTSTRUCTW tv{};
        tv.hParent = parent;
        tv.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        tv.item.pszText = const_cast<LPWSTR>(name.c_str());
        tv.item.lParam = reinterpret_cast<LPARAM>(new std::wstring(path));
        tv.item.iImage = tv.item.iSelectedImage = 0;
        tv.item.cChildren = 1;
        return TreeView_InsertItem(tree_, &tv);
    };

    wchar_t* p = drives;
    while (*p) {
        std::wstring drive = p;
        if (drive.size() >= 2 && drive[1] == L'\\') drive.resize(2); // "C:"
        std::wstring drivePath = drive + L"\\";
        int icon = shell::SysIconIndexForEntry(drivePath, true);
        TVINSERTSTRUCTW tv{};
        tv.hParent = TVI_ROOT;
        tv.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        tv.item.pszText = const_cast<LPWSTR>(drivePath.c_str());
        tv.item.lParam = reinterpret_cast<LPARAM>(new std::wstring(drivePath));
        tv.item.iImage = tv.item.iSelectedImage = icon >= 0 ? icon : 0;
        tv.item.cChildren = 1;
        TreeView_InsertItem(tree_, &tv);
        p += wcslen(p) + 1;
    }
}

// ---------------------------------------------------------------------------
// 树懒展开：节点首次展开时才枚举其子目录
// ---------------------------------------------------------------------------

// 枚举 dir 的子文件夹插入 parent 节点下；返回是否真的有子文件夹
static bool InsertChildFolders(HWND tree, HTREEITEM parent, const std::wstring& dir)
{
    std::wstring pattern = dir;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchLimitToDirectories, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool any = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        // 跳过隐藏/系统目录（AppData 等太密），保持树干净
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;

        std::wstring name = fd.cFileName;
        std::wstring path = dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
        int icon = shell::SysIconIndexForEntry(path, true);

        TVINSERTSTRUCTW tv{};
        tv.hParent = parent;
        tv.hInsertAfter = TVI_SORT;
        tv.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        tv.item.pszText = const_cast<LPWSTR>(name.c_str());
        tv.item.lParam = reinterpret_cast<LPARAM>(new std::wstring(path));
        tv.item.iImage = tv.item.iSelectedImage = icon >= 0 ? icon : 0;
        tv.item.cChildren = 1; // 先假设有子目录，靠占位节点显示展开箭头
        TreeView_InsertItem(tree, &tv);
        any = true;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

void MainWindow::ExpandTreeNode(HTREEITEM item)
{
    // 已展开过（子节点不是占位符）就不再枚举
    TVITEMW it{};
    it.hItem = TreeView_GetChild(tree_, item);
    it.mask = TVIF_PARAM;
    if (it.hItem && TreeView_GetItem(tree_, &it) && it.lParam)
        return; // 已有真实子节点

    auto* path = reinterpret_cast<std::wstring*>([&] {
        TVITEMW self{}; self.hItem = item; self.mask = TVIF_PARAM;
        TreeView_GetItem(tree_, &self);
        return self.lParam;
    }());
    if (!path) return;

    // 清掉占位节点（TVI_SORT 插入后占位在最后，但保险起见全删）
    for (HTREEITEM c = TreeView_GetChild(tree_, item); c; ) {
        HTREEITEM next = TreeView_GetNextSibling(tree_, c);
        auto* p = reinterpret_cast<std::wstring*>([&] {
            TVITEMW ci{}; ci.hItem = c; ci.mask = TVIF_PARAM;
            TreeView_GetItem(tree_, &ci);
            return ci.lParam;
        }());
        delete p;
        TreeView_DeleteItem(tree_, c);
        c = next;
    }

    if (!InsertChildFolders(tree_, item, *path)) {
        // 没有子目录：撤掉展开箭头
        TVITEMW fix{};
        fix.hItem = item;
        fix.mask = TVIF_CHILDREN;
        fix.cChildren = 0;
        TreeView_SetItem(tree_, &fix);
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
void MainWindow::OnDelete()
{
    std::wstring path;
    if (!SelectedPath(path)) return;
    if (shell::ExecuteFileOp(hwnd_, shell::FileOp::Delete, path, L""))
        RefreshList();
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
    if (!SelectedPath(path)) return;
    std::wstring oldName = path.substr(path.find_last_of(L'\\') + 1);

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
    }

    g_renameConfirmed = false;
    g_renameText.clear();

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, DLG_CLASS, L"重命名",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 420, 112, hwnd_, nullptr, hInst, nullptr);
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

    if (!g_renameConfirmed || g_renameText.empty() || g_renameText == oldName) return;

    if (shell::ExecuteFileOp(hwnd_, shell::FileOp::Rename, path, g_renameText))
        RefreshList();
}

void MainWindow::OnClipboard(bool cut, bool copyOnly)
{
    std::wstring path;
    if (!SelectedPath(path)) return;
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 2) * sizeof(wchar_t));
    if (h) {
        auto* p = static_cast<wchar_t*>(GlobalLock(h));
        wcscpy_s(p, path.size() + 2, path.c_str());
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    // 记录 cut/copy 状态供粘贴使用（简化：静态存储）
    static std::wstring clipPath;
    static bool clipCut = false;
    clipPath = path;
    clipCut = cut && !copyOnly;
    (void)clipPath; (void)clipCut;
}

void MainWindow::OnPaste()
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(hwnd_)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    std::wstring src = h ? static_cast<const wchar_t*>(GlobalLock(h)) : L"";
    if (h) GlobalUnlock(h);
    CloseClipboard();
    if (src.empty()) return;
    if (shell::ExecuteFileOp(hwnd_, shell::FileOp::Copy, src, CurTab().dir))
        RefreshList();
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
static LRESULT CALLBACK PaneTabProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (!IsWindow(h)) return 0; // 窗格被销毁后可能还有残余消息，别再往下走
    WNDPROC orig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!orig) return DefWindowProcW(h, m, wp, lp);
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(GetParent(h), GWLP_USERDATA));
    if (self) return self->PaneTabHandler(h, m, wp, lp, orig);
    return CallWindowProcW(orig, h, m, wp, lp);
}

LRESULT MainWindow::PaneTabHandler(HWND h, UINT m, WPARAM wp, LPARAM lp, WNDPROC orig)
{
    switch (m) {
    case WM_LBUTTONDOWN: {
        TCHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = (int)SendMessageW(h, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
        if (idx >= 0) {
            // 记录拖拽起点，但不拦截：继续走默认过程完成 tab 切换
            for (size_t pi = 0; pi < panes_.size(); ++pi) {
                if (panes_[pi].tab == h) {
                    tabDragActive_ = true;
                    tabDragPane_ = (int)pi;
                    tabDragIndex_ = idx;
                    SetCapture(h);
                    break;
                }
            }
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
                    if (tabIndex != SIZE_MAX && tabs_[tabIndex].locked)
                        tabIndex = SIZE_MAX;    // 锁定的分页不允许被拖走
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

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
void MainWindow::Layout()
{
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    int y = 0;

    auto place = [&](HWND h, int x, int yy, int w, int hh) {
        MoveWindow(h, x, yy, w, hh, TRUE);
    };

    // 工具栏行
    place(btnBack_,   4, y + 4, 40, 26);
    place(btnFwd_,   48, y + 4, 40, 26);
    place(btnUp_,    92, y + 4, 40, 26);
    place(btnRefresh_,136, y + 4, 50, 26);
    place(address_,  194, y + 6, W - 200, 24);
    y += 36;

    // 状态栏占据底部一条，先量出它的高度，分页栏放在它上面
    RECT rs; SendMessageW(status_, WM_SIZE, 0, MAKELPARAM(W, H));
    GetWindowRect(status_, &rs);
    int statusH = rs.bottom - rs.top;

    int pagerY = H - statusH - 28;
    int listH = pagerY - y;

    // 左侧 Tab 容器：先放 tab，再把内容页放到 tab 显示区内
    // （tree_/favList_ 是 tab_ 的子窗口，坐标相对 tab_ 客户区）
    int sideW = sideWidth_;
    place(tab_, 0, y, sideW, listH);
    RECT rt{};
    SendMessageW(tab_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rt));
    int tabH = rt.bottom - rt.top;
    int innerH = listH - tabH - 12;
    place(tree_, 4, tabH + 6, sideW - 8, innerH);
    place(favList_, 4, tabH + 6, sideW - 8, innerH);

    // 右侧多窗格网格布局：
    //   1 个：独占；2 个：左右；3 个：品/倒品（triLayout_）；4 个：田字形
    //   同排两个窗格的宽度按 Pane::width 比例分配（分隔条可拖）
    int gx = sideW + 12;
    int gw = W - gx;
    int gh = listH;
    const int S = 12; // 分隔条厚度
    paneRects_.clear();
    splitPairs_.clear();

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
        int halfH = (gh - S) / 2;
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
        int halfH = (gh - S) / 2;
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

        // “+” 按钮放在该窗格最后一个 tab 头右侧
        RECT rl{};
        int cnt = (int)SendMessageW(p.tab, TCM_GETITEMCOUNT, 0, 0);
        if (cnt > 0) SendMessageW(p.tab, TCM_GETITEMRECT, cnt - 1, reinterpret_cast<LPARAM>(&rl));
        p.lastTabRight = r.left + rl.right + 6;
    }

    // “+” 按钮跟随激活窗格
    if (activePane_ < panes_.size())
        place(btnNewTab_, panes_[activePane_].lastTabRight, y + 3, 26, 22);

    // 记录分隔条可拖动的水平区间（供命中测试）
    splitTop_ = y;
    splitBot_ = pagerY;

    // 分页栏
    int px = 4;
    place(pagerFirst_, px, pagerY + 2, 40, 24); px += 44;
    place(pagerPrev_,  px, pagerY + 2, 40, 24); px += 44;
    place(pagerLabel_, px, pagerY + 4, 160, 20); px += 164;
    place(pagerNext_,  px, pagerY + 2, 40, 24); px += 44;
    place(pagerLast_,  px, pagerY + 2, 40, 24); px += 48;
    place(pagerSize_,  px, pagerY + 2, 100, 24);

    // 状态栏自动布局
    SendMessageW(status_, WM_SIZE, 0, MAKELPARAM(W, H));
}

LRESULT MainWindow::ListViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    int pi = PaneOfList(h);
    if (pi >= 0 && msg == WM_CONTEXTMENU) {
        // 右键菜单：在列表空白/条目上弹出 Explorer 风格菜单
        if ((size_t)pi != activePane_) SelectPane((size_t)pi); // 先激活该窗格
        int sel = ListView_GetNextItem(h, -1, LVNI_SELECTED);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) { // 键盘触发
            pt = { 200, 200 };
            ClientToScreen(hwnd_, &pt);
        }
        std::wstring path;
        if (sel >= 0) path = PaneItemPath((size_t)pi, sel);
        HMENU menu = CreatePopupMenu();
        if (!path.empty())
            AppendMenuW(menu, MF_STRING, 1, L"添加当前目录到收藏");
        else
            AppendMenuW(menu, MF_STRING, 1, L"添加此目录到收藏");
        int addCmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, hwnd_, nullptr);
        DestroyMenu(menu);
        if (addCmd == 1) { OnAddFavorite(); return 0; }
        if (shell::ShowContextMenu(hwnd_, path, path.empty() ? CurTab().dir : L"", pt))
            RefreshList();
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_DELETE) { /* Del 经 LVN_KEYDOWN 处理 */ }
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
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
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
            // 操作非激活窗格 -> 先激活它（重绘除外，避免中途切换数据源造成重入）
            if ((size_t)pi != activePane_ && nm->code != LVN_GETDISPINFOW)
                SelectPane((size_t)pi);
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
                        else
                            ShellExecuteW(hwnd_, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
            }
            else if (nm->code == LVN_COLUMNCLICK) {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
                OnColumnClick(lv->iSubItem);
            }
            else if (nm->code == LVN_KEYDOWN) {
                auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
                if (kd->wVKey == VK_DELETE) OnDelete();
                else if (kd->wVKey == VK_F2) OnRename();
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
                    if (dir && !dir->empty()) Navigate(*dir);
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
                    if (dir && !dir->empty()) Navigate(*dir);
                } else if (cmd == 2) {
                    OnRemoveFavorite();
                }
            }
        }
        else if (nm->idFrom == IDC_TREE) {
            if (nm->code == TVN_ITEMEXPANDINGW) {
                auto* ti = reinterpret_cast<NMTREEVIEWW*>(nm);
                if (ti->action & TVE_EXPAND)
                    ExpandTreeNode(ti->itemNew.hItem);
            }
            else if (nm->code == TVN_SELCHANGEDW) {
                auto* ti = reinterpret_cast<NMTREEVIEWW*>(nm);
                auto* path = reinterpret_cast<std::wstring*>(ti->itemNew.lParam);
                if (path && !path->empty()) Navigate(*path);
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
            const std::wstring& dir = CurTab().dir;
            size_t p = dir.find_last_of(L'\\');
            if (p != std::wstring::npos && p > 2) Navigate(dir.substr(0, p));
            return 0;
        }
        case IDC_NEWTAB:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && panes_.size() < 4)
                AddPane();          // Ctrl+点击：新增窗格（最多 4 个）
            else
                AddRightTab(true);  // 普通点击：当前窗格加一个分页
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
        case IDC_REFRESH: RefreshList(); return 0;
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
                pageSize_ = (sel == 0) ? 50 : (sel == 2) ? 200 : (sel == 3) ? 500 : 100;
                CurTab().curPage = 0;
                Navigate(CurTab().dir, false);
            }
            return 0;
        case IDC_ADDRESS:
            if (HIWORD(wp) == EN_KILLFOCUS || HIWORD(wp) == EN_ADDR_RETURN) {
                wchar_t buf[MAX_PATH * 2] = {};
                GetWindowTextW(address_, buf, MAX_PATH * 2);
                if (buf[0] && buf != CurTab().dir) Navigate(buf);
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
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            // 与 WM_APP_WIN_E 相同：借前台线程输入权限置前
            HWND fg = GetForegroundWindow();
            DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            DWORD myTid = GetCurrentThreadId();
            bool attached = fgTid && fgTid != myTid &&
                            AttachThreadInput(myTid, fgTid, TRUE);
            BringWindowToTop(hwnd_);
            SetForegroundWindow(hwnd_);
            if (attached) AttachThreadInput(myTid, fgTid, FALSE);
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
        // 窗格间分隔条命中：splitPairs_ 各对的右缘 .. +12（限该排高度内）
        if (yy >= splitTop_ && yy <= splitBot_) {
            for (size_t k = 0; k < splitPairs_.size(); ++k) {
                auto& pr = splitPairs_[k];
                int edge = paneRects_[pr.first].right;
                int top = paneRects_[pr.first].top, bot = paneRects_[pr.first].bottom;
                if (x >= edge && x <= edge + 12 && yy >= top && yy <= bot) {
                    paneSplitDragging_ = true;
                    paneSplitIndex_ = (int)k;
                    SetCapture(hwnd_);
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return 0;
                }
            }
        }
        if (x >= sideWidth_ && x <= sideWidth_ + 12 && yy >= splitTop_ && yy <= splitBot_) {
            draggingSplitter_ = true;
            SetCapture(hwnd_);
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
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
                int maxW = paneRects_[b].right - minW - 12;
                if (want < minW) want = minW;
                if (want > maxW) want = maxW;
                int delta = want - panes_[a].width;
                panes_[a].width += delta;
                panes_[b].width -= delta;
                if (panes_[b].width < 160) panes_[b].width = 160;
                Layout();
            }
            return 0;
        }
        if (draggingSplitter_) {
            int x = static_cast<int>(GET_X_LPARAM(lp));
            RECT rc; GetClientRect(hwnd_, &rc);
            int maxW = rc.right - 260;                 // 右侧至少留 260px
            sideWidth_ = (x < 150) ? 150 : (x > maxW ? maxW : x);
            Layout();
            return 0;
        }
        break;
    case WM_LBUTTONUP:
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
        // 窗格间分隔条光标
        if (pt.y >= splitTop_ && pt.y <= splitBot_) {
            for (size_t k = 0; k < splitPairs_.size(); ++k) {
                auto& pr = splitPairs_[k];
                int edge = paneRects_[pr.first].right;
                if (pt.x >= edge && pt.x <= edge + 12 &&
                    pt.y >= paneRects_[pr.first].top && pt.y <= paneRects_[pr.first].bottom) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
        }
        if (pt.x >= sideWidth_ && pt.x <= sideWidth_ + 12 &&
            pt.y >= splitTop_ && pt.y <= splitBot_) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;  // 其它区域交给 DefWindowProc 设置默认光标
    }

    case WM_DESTROY:
        for (auto& t : tabs_) t.pages->Shutdown();
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
