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

// 控件 ID
enum {
    IDC_ADDRESS = 1001, IDC_TREE = 1002, IDC_LIST = 1003,
    IDC_BACK = 1004, IDC_FORWARD = 1005, IDC_UP = 1006, IDC_REFRESH = 1007,
    IDC_FIRST = 1010, IDC_PREV = 1011, IDC_NEXT = 1012, IDC_LAST = 1013,
    IDC_PAGE_SIZE = 1014, IDC_PAGER_LABEL = 1015,
    IDC_TAB = 1016, IDC_FAVLIST = 1017,
};

// 自定义通知值：Edit 没有 EN_RETURN 常量，回车通知用这个
static constexpr UINT EN_ADDR_RETURN = 0x1000;

static LRESULT CALLBACK AddressProc(HWND h, UINT m, WPARAM wp, LPARAM lp); // 前向声明

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
    self->pages_.SetNotify([self]() {
        PostMessage(self->hwnd_, WM_APP_PAGELOADED, 0, 0);
    });

    RECT rcWork{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int w = rcWork.right - rcWork.left - 120, h = rcWork.bottom - rcWork.top - 120;

    self->hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"分页资源管理器 - Paged Explorer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, hInst, self);
    if (!self->hwnd_) { delete self; return nullptr; }

    self->BuildChildren();
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

    // 文件列表
    CreateListView();

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

void MainWindow::BuildImageList()
{
    // 用系统图像列表（Shell 维护，按 SHGFI_SYSICONINDEX 索引取图标）
    SHFILEINFOW fi{};
    HIMAGELIST sys = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    if (sys) {
        ListView_SetImageList(list_, sys, LVSIL_SMALL);
    } else {
        int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
        imgList_ = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 32, 64);
        ListView_SetImageList(list_, imgList_, LVSIL_SMALL);
    }
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

void MainWindow::CreateListView()
{
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SHAREIMAGELISTS,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);
    if (uiFont_) SendMessageW(list_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    BuildImageList();
    InsertColumns();

    listOldProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(list_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&MainWindow::ListViewProcStatic)));
    SetWindowLongPtrW(list_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void MainWindow::InsertColumns()
{
    auto add = [&](int idx, const wchar_t* text, int w) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, LVCFMT_LEFT, w, const_cast<LPWSTR>(text) };
        c.iSubItem = idx;
        ListView_InsertColumn(list_, idx, &c);
    };
    add(COL_NAME, L"名称", 300);
    add(COL_TYPE, L"类型", 140);
    add(COL_SIZE, L"大小", 110);
    add(COL_MTIME, L"修改日期", 160);
}

// ---------------------------------------------------------------------------
// 导航
// ---------------------------------------------------------------------------
void MainWindow::Navigate(const std::wstring& path, bool addHistory)
{
    curDir_ = path;
    curPage_ = 0;
    pageItems_.clear();
    SetWindowTextW(address_, path.c_str());

    if (addHistory) {
        if (!history_.empty() && histPos_ >= 0 && history_[histPos_] == path) {
            // same
        } else {
            history_.erase(history_.begin() + histPos_ + 1, history_.end());
            history_.push_back(path);
            histPos_ = static_cast<int>(history_.size()) - 1;
        }
    }

    pages_.OpenDirectory(path, pageSize_);
    ListView_SetItemCountEx(list_, 0, 0);
    UpdateStatusBar();
    UpdatePaginationBar();
}

void MainWindow::RefreshList()
{
    pageItems_.clear();
    ListView_SetItemCountEx(list_, 0, 0);
    pages_.RequestPage(curPage_); // 可能命中缓存，也可能后台加载
    if (pages_.TryGetPage(curPage_, pageItems_)) {
        ApplyCurrentSort(); // 套用已保存的排序
        ListView_SetItemCountEx(list_, pageItems_.size(), LVSICF_NOINVALIDATEALL);
        ListView_RedrawItems(list_, 0, static_cast<int>(pageItems_.size()) - 1);
    }
    UpdateStatusBar();
    UpdatePaginationBar();
}

void MainWindow::OnPageLoaded()
{
    // 后台加载完成后：如果当前页还没内容就填充
    if (pageItems_.empty() && pages_.TryGetPage(curPage_, pageItems_)) {
        ApplyCurrentSort(); // 套用已保存的排序
        ListView_SetItemCountEx(list_, pageItems_.size(), LVSICF_NOINVALIDATEALL);
        ListView_RedrawItems(list_, 0, static_cast<int>(pageItems_.size()) - 1);
    }
    pages_.PrefetchAround(curPage_);
    UpdateStatusBar();
    UpdatePaginationBar();
}

void MainWindow::UpdateStatusBar()
{
    wchar_t buf[160];
    unsigned long long total = pages_.TotalCount();
    if (total > 0)
        wsprintfW(buf, L"共 %I64u 项   第 %I64u / %zu 页   每页 %zu 项",
            total, static_cast<unsigned long long>(curPage_ + 1), pages_.PageCount(), pageSize_);
    else
        wsprintfW(buf, L"正在统计 %s ...", curDir_.c_str());
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(buf));
}

void MainWindow::UpdatePaginationBar()
{
    wchar_t buf[80];
    unsigned long long total = pages_.TotalCount();
    if (total > 0)
        wsprintfW(buf, L"第 %I64u / %zu 页", static_cast<unsigned long long>(curPage_ + 1), pages_.PageCount());
    else
        wsprintfW(buf, L"加载中...");
    SetWindowTextW(pagerLabel_, buf);
    EnableWindow(pagerFirst_, curPage_ > 0);
    EnableWindow(pagerPrev_, curPage_ > 0);
    EnableWindow(pagerNext_, total > 0 && (curPage_ + 1) < pages_.PageCount());
    EnableWindow(pagerLast_, total > 0 && (curPage_ + 1) < pages_.PageCount());
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
    std::sort(pageItems_.begin(), pageItems_.end(), [&](const FileEntry& a, const FileEntry& b) {
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
    if (!pageItems_.empty())
        ListView_RedrawItems(list_, 0, static_cast<int>(pageItems_.size()) - 1);
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
    if (item < 0 || item >= static_cast<int>(pageItems_.size())) return {};
    return pageItems_[item].path;
}

bool MainWindow::SelectedPath(std::wstring& out) const
{
    int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (sel < 0) return false;
    out = CurrentPagePath(sel);
    return !out.empty();
}

// ---------------------------------------------------------------------------
// 树
// ---------------------------------------------------------------------------
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

void MainWindow::LoadFavorites()
{
    favorites_.clear();
    std::wstring path = FavoritesFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) { RefreshFavoritesList(); return; }
    // UTF-8 逐行读取；首行可为 sort=列,方向 配置行
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, line, (int)n, nullptr, 0);
        if (wlen <= 0) continue;
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line, (int)n, w.data(), wlen);
        if (first) {
            first = false;
            if (w.rfind(L"sort=", 0) == 0) {
                int col = 0, asc = 1;
                swscanf_s(w.c_str() + 5, L"%d,%d", &col, &asc);
                if (col >= 0 && col <= 3) { sortCol_ = col; sortAsc_ = asc != 0; }
                continue;
            }
            // 首行不是配置行，当作收藏路径处理
        }
        favorites_.push_back(std::move(w));
    }
    fclose(f);
    RefreshFavoritesList();
}

void MainWindow::SaveFavorites()
{
    std::wstring path = FavoritesFilePath();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    // 写 UTF-8 BOM + 配置行 + UTF-8 行
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, f);
    char cfg[64];
    int cn = WideCharToMultiByte(CP_UTF8, 0,
        (L"sort=" + std::to_wstring(sortCol_) + L"," + std::to_wstring(sortAsc_ ? 1 : 0)).c_str(), -1,
        cfg, sizeof(cfg) - 1, nullptr, nullptr);
    if (cn > 0) { cfg[cn - 1] = '\n'; fwrite(cfg, 1, cn, f); }
    for (const auto& dir : favorites_) {
        char buf[2048];
        int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(),
                                    buf, sizeof(buf) - 2, nullptr, nullptr);
        if (n > 0) {
            buf[n++] = '\n';
            fwrite(buf, 1, n, f);
        }
    }
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
    if (curDir_.empty()) return;
    // 去重
    for (const auto& d : favorites_)
        if (_wcsicmp(d.c_str(), curDir_.c_str()) == 0) return;
    favorites_.push_back(curDir_);
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
    SaveFavorites();
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
    SetFocus(list_);

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
    if (shell::ExecuteFileOp(hwnd_, shell::FileOp::Copy, src, curDir_))
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
    place(tab_, 0, y, 220, listH);
    RECT rt{};
    SendMessageW(tab_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rt));
    int tabH = rt.bottom - rt.top;
    int innerY = y + tabH + 6;
    int innerH = listH - tabH - 12;
    place(tree_, 4, innerY, 212, innerH);
    place(favList_, 4, innerY, 212, innerH);

    place(list_, 224, y, W - 224, listH);

    // 分页栏
    int x = 4;
    place(pagerFirst_, x, pagerY + 2, 40, 24); x += 44;
    place(pagerPrev_,  x, pagerY + 2, 40, 24); x += 44;
    place(pagerLabel_, x, pagerY + 4, 160, 20); x += 164;
    place(pagerNext_,  x, pagerY + 2, 40, 24); x += 44;
    place(pagerLast_,  x, pagerY + 2, 40, 24); x += 48;
    place(pagerSize_,  x, pagerY + 2, 100, 24);

    // 状态栏自动布局
    SendMessageW(status_, WM_SIZE, 0, MAKELPARAM(W, H));
}

LRESULT MainWindow::ListViewProc(UINT msg, WPARAM wp, LPARAM lp)
{
    // 右键菜单：在列表空白/条目上弹出 Explorer 风格菜单
    if (msg == WM_CONTEXTMENU) {
        int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) { // 键盘触发
            pt = { 200, 200 };
            ClientToScreen(hwnd_, &pt);
        }
        std::wstring path;
        if (sel >= 0) path = CurrentPagePath(sel);
        HMENU menu = CreatePopupMenu();
        if (!path.empty())
            AppendMenuW(menu, MF_STRING, 1, L"添加当前目录到收藏");
        else
            AppendMenuW(menu, MF_STRING, 1, L"添加此目录到收藏");
        int addCmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, hwnd_, nullptr);
        DestroyMenu(menu);
        if (addCmd == 1) { OnAddFavorite(); return 0; }
        if (shell::ShowContextMenu(hwnd_, path, path.empty() ? curDir_ : L"", pt))
            RefreshList();
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_DELETE) { /* Del 经主窗口加速键 */ }
    return CallWindowProcW(listOldProc_, list_, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::ListViewProcStatic(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                                UINT_PTR, DWORD_PTR ref)
{
    // 用 GWLP_USERDATA 取 self（CreateListView 时已设置）
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->ListViewProc(m, wp, lp);
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == IDC_LIST) {
            if (nm->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
                int i = di->item.iItem;
                if (i < 0 || i >= (int)pageItems_.size()) return 0;
                FileEntry& e = pageItems_[i];
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
        else if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            SwitchSideTab((int)SendMessageW(tab_, TCM_GETCURSEL, 0, 0));
        }
        else if (nm->idFrom == IDC_FAVLIST) {
            if (nm->code == NM_DBLCLK) {
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
        case IDC_BACK:
            if (histPos_ > 0) { --histPos_; Navigate(history_[histPos_], false); }
            return 0;
        case IDC_FORWARD:
            if (histPos_ + 1 < (int)history_.size()) { ++histPos_; Navigate(history_[histPos_], false); }
            return 0;
        case IDC_UP: {
            size_t p = curDir_.find_last_of(L'\\');
            if (p != std::wstring::npos && p > 2) Navigate(curDir_.substr(0, p));
            return 0;
        }
        case IDC_REFRESH: RefreshList(); return 0;
        case IDC_FIRST: if (curPage_ != 0) { curPage_ = 0; RefreshList(); } return 0;
        case IDC_PREV:  if (curPage_ > 0) { --curPage_; RefreshList(); } return 0;
        case IDC_NEXT:
            if (curPage_ + 1 < pages_.PageCount()) { ++curPage_; RefreshList(); }
            return 0;
        case IDC_LAST:
            if (pages_.PageCount() > 0) { curPage_ = pages_.PageCount() - 1; RefreshList(); }
            return 0;
        case IDC_PAGE_SIZE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(pagerSize_, CB_GETCURSEL, 0, 0);
                pageSize_ = (sel == 0) ? 50 : (sel == 2) ? 200 : (sel == 3) ? 500 : 100;
                curPage_ = 0;
                Navigate(curDir_, false);
            }
            return 0;
        case IDC_ADDRESS:
            if (HIWORD(wp) == EN_KILLFOCUS || HIWORD(wp) == EN_ADDR_RETURN) {
                wchar_t buf[MAX_PATH * 2] = {};
                GetWindowTextW(address_, buf, MAX_PATH * 2);
                if (buf[0] && buf != curDir_) Navigate(buf);
                if (HIWORD(wp) == EN_ADDR_RETURN) SetFocus(list_);
            }
            return 0;
        }
        return 0;
    }

    case WM_APP_PAGELOADED:
        OnPageLoaded();
        return 0;

    case WM_DESTROY:
        pages_.Shutdown();
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
