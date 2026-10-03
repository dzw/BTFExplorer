#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <memory>
#include <vector>
#include "../Pagination/PageManager.h"
#include "../FileModel/FileEntry.h"
#include "DirectoryTree.h"
#include "TrayIcon.h"

// 控件 ID（主窗口与 main.cpp 的加速键 / 单实例逻辑共享）
enum {
    IDC_ADDRESS = 1001, IDC_TREE = 1002, IDC_LIST = 1003,
    IDC_BACK = 1004, IDC_FORWARD = 1005, IDC_UP = 1006, IDC_REFRESH = 1007,
    IDC_FIRST = 1010, IDC_PREV = 1011, IDC_NEXT = 1012, IDC_LAST = 1013,
    IDC_PAGE_SIZE = 1014, IDC_PAGER_LABEL = 1015,
    IDC_TAB = 1016, IDC_FAVLIST = 1017, IDC_RIGHTTAB = 1018, IDC_NEWTAB = 1019,
    IDC_NEWTAB_BASE = 1200,    // 每个窗格一个“+”按钮，命令 ID = IDC_NEWTAB_BASE + 窗格 tag
    IDC_TOOLS_BASE = 1210,     // 每个窗格一个“▾”外部工具按钮，命令 ID = IDC_TOOLS_BASE + tag
    IDC_LAYOUT1 = 1020, IDC_LAYOUT2 = 1021, IDC_LAYOUT3 = 1022, IDC_LAYOUT4 = 1023,
    IDC_TRI_PINTOP = 1024, IDC_TRI_PINDOWN = 1025,
    IDC_MENU_FILTERS = 1026,
    IDC_TREE_SYNC = 1027,
    IDC_SETTINGS = 1028,   // 工具栏“设置”按钮（对话框控件 ID 见 SettingsDialog.cpp）
    // 分页标题右键菜单：关闭 / 关闭其他 / 关闭右边 / 锁定
    IDC_TM_LOCK = 1030, IDC_TM_CLOSE = 1031, IDC_TM_OTHERS = 1032, IDC_TM_RIGHT = 1033,
};

// 每个右侧分页的独立状态（目录/页码/历史/加载器各自独立）
struct TabState {
    std::wstring dir;
    std::vector<std::wstring> history;
    int histPos = -1;
    std::vector<FileEntry> pageItems;
    size_t curPage = 0;
    size_t pane = 0;                         // 所属窗格
    bool locked = false;                     // 锁定：不被批量/中键关闭
    std::unique_ptr<PageManager> pages;
    TabState() : pages(std::make_unique<PageManager>()) {}
};

// 右侧窗格：一个窗格 = 一个 Tab 容器 + 一个虚拟列表，可水平摆放多个
static constexpr int IDC_LIST_BASE = 1100;   // 窗格列表的 ID = IDC_LIST_BASE + tag
struct Pane {
    HWND tab = nullptr;                      // 该窗格的 Tab 容器
    HWND list = nullptr;                     // 该窗格的虚拟 ListView
    WNDPROC listOld = nullptr;               // 列表原窗口过程（每个窗格各自一份）
    int tag = -1;                            // 窗格唯一编号（控件 ID 后缀，删除后编号可复用）
    int width = 340;                         // 窗格宽度（分隔条可拖）
    std::vector<size_t> tabs;                // 本窗格持有的 TabState 下标
    size_t active = 0;                       // 本窗格当前显示的分页
    int lastTabRight = 0;                    // 最后一个 tab 头右缘（“+”按钮定位用）
    HWND btnNewTab = nullptr;                // 本窗格的“+”新增分页按钮
    HWND btnTools = nullptr;                 // 本窗格右上角的“▾”外部工具按钮
};

// 资源管理器主窗口：树 + 虚拟 ListView + 地址栏 + 分页栏 + 状态栏
class MainWindow {
public:
    static MainWindow* Create(HINSTANCE hInst);
    PageManager& Pages() { return *tabs_[activeTab_].pages; }
    HWND Hwnd() const { return hwnd_; } // main.cpp 单实例/加速键/Win+E 拦截用
    // 打开外部传入的目录或文件（文件则导航到其所在目录）
    void OpenTarget(const std::wstring& path);

private:
    MainWindow() = default;

    friend LRESULT CALLBACK PaneTabProc(HWND, UINT, WPARAM, LPARAM);

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListViewProcStatic(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(UINT, WPARAM, LPARAM);
    LRESULT ListViewProc(HWND h, UINT, WPARAM, LPARAM);
    LRESULT PaneTabHandler(HWND h, UINT m, WPARAM wp, LPARAM lp, WNDPROC orig); // 分页拖拽

    // UI 构建
    void BuildChildren();
    void BuildImageList(HWND list);  // 给指定列表挂上（共用的）系统图像列表
    void Layout();
    void CreateSidePanel();          // 左侧 Tab 容器：目录树 / 收藏
    void SwitchSideTab(int index);   // 切换 tab 显示
    void SyncTreeToCurrentTab(bool showErrors = true);
    void CreatePane(Pane& p);        // 创建一个窗格（tab 容器 + 虚拟列表）
    void Navigate(const std::wstring& path, bool addHistory = true);
    void NavigateFromSidebar(const std::wstring& path); // 侧栏跳转：分页被锁定时另开新分页

    // 列表（作用于激活窗格的当前分页）
    HWND  CurList() const;            // 激活窗格的 ListView
    void RefreshList();               // 重新请求当前页
    void OnPageLoaded();              // 后台加载完成（已 PostMessage 转到 UI 线程）
    void UpdateStatusBar();
    void UpdatePaginationBar();
    void OnColumnClick(int col);
    void ApplyCurrentSort();
    void InsertColumns(HWND list);   // 给指定列表建列
    DWORD ListExStyle() const;       // 文件列表的扩展样式（受“显示网格线”开关控制）
    void ApplyListStyles();          // 把当前开关状态套用到所有窗格列表
    void SyncPagerSizeCombo();       // 每页项数变化/恢复后，同步分页栏下拉框
    int  EnsureIcon(FileEntry& e);       // 系统图像列表索引（懒取并缓存）
    bool SelectedPath(std::wstring& out) const;
    std::vector<std::wstring> SelectedPaths() const; // 选中项完整路径（支持多选）
    std::wstring CurrentPagePath(int item) const; // item -> full path

    // 收藏
    void LoadFavorites();
    void SaveFavorites();          // 保存（带防丢失保护：内存为空时保住磁盘收藏）
    void SaveFavoritesCore(bool allowEmptyFavorites); // 显式删除最后一条时传 true
    bool ReadFavoritesFromDisk(std::vector<std::wstring>& out); // 整文件读入，解析成功才返回 true
    void RefreshFavoritesList();
    void OnAddFavorite();
    void OnRemoveFavorite();

    // 键盘/命令
    void OnDelete(bool toRecycleBin = true); // 删除选中项；false=不进回收站（Shift+Delete）
    void OnRename();
    void OnClipboard(bool cut);      // 复制/剪切选中项（CF_HDROP + Preferred DropEffect）
    void OnPaste();                  // 粘贴剪贴板中的文件到当前目录
    void HideToTray();
    void ShowFromTray();
    void EnsureTrayIcon();      // 图标常驻：确保通知区里有图标（幂等）
    void OpenSettings();

    // 数据
    HWND hwnd_ = nullptr, address_ = nullptr;
    TrayIcon trayIcon_;
    DirectoryTree directoryTree_;
    HWND tab_ = nullptr, favList_ = nullptr, btnTreeSync_ = nullptr; // 左侧 Tab / 收藏 / 树同步
    std::vector<std::wstring> favorites_;      // 收藏的目录路径
    HWND status_ = nullptr, pagerPrev_ = nullptr, pagerNext_ = nullptr;
    HWND pagerFirst_ = nullptr, pagerLast_ = nullptr, pagerLabel_ = nullptr, pagerSize_ = nullptr;
    HWND btnBack_ = nullptr, btnFwd_ = nullptr, btnUp_ = nullptr, btnRefresh_ = nullptr;
    HWND btnMenuFilters_ = nullptr, btnSettings_ = nullptr;
    HIMAGELIST imgList_ = nullptr;   // 取不到系统图像列表时的自建兜底
    HIMAGELIST sysImgs_ = nullptr;   // 系统图像列表（所有窗格共用）
    HFONT uiFont_ = nullptr;

    // 布局 / 交互
    int sideWidth_ = 220;          // 左侧面板宽度（可被分隔条拖动改变）
    int startupShowCmd_ = SW_SHOW;
    bool restoreInProgress_ = false;     // 恢复会话期间抑制 SaveSession，避免递归/重复写盘
    RECT lastSavedWinRect_{};            // 上次落盘的窗口矩形（用于跳过重排时的重复写）
    DWORD lastSaveTick_ = 0;             // 上次落盘时刻（限流，避免拖动时频繁写盘）
    bool draggingSplitter_ = false;
    int splitTop_ = 36;            // 分隔条可拖动的水平范围
    int splitBot_ = 0;

    // ---- 多窗格 / 多分页状态 ----
    std::vector<TabState> tabs_;        // 全部分页（按 pane 字段归属窗格）
    std::vector<Pane> panes_;           // 右侧窗格（水平排列）
    size_t activePane_ = 0;             // 激活窗格（树/收藏/命令作用目标）
    size_t activeTab_ = 0;              // 激活窗格内的当前分页（tabs_ 下标）
    bool paneSplitDragging_ = false;    // 正在拖窗格间分隔条
    int  paneSplitIndex_ = -1;          // 拖动的分隔条（splitPairs_ 下标）
    std::vector<RECT> paneRects_;                // 各窗格客户区矩形（命中测试/拖拽用）
    std::vector<std::pair<int,int>> splitPairs_; // 同排并排窗格对（边界可拖）
    int layoutCount_ = 1;               // 窗格数 1~4（Alt+1~4 切换，= panes_.size()）
    int triLayout_ = 1;                 // 3 窗格形态：0=品字形(1上2下) 1=倒品字形(2上1下)，默认倒品
    int  savedPaneCount_ = 1;           // favorites.txt 的 panes= 恢复值
    bool startupLayoutApplied_ = false; // 启动恢复只做一次
    size_t pendingCloseTab_ = SIZE_MAX; // 中键点击待关闭的分页（延后到下一次消息循环）
    size_t pendingMoveTab_ = SIZE_MAX;  // 拖拽待移动的分页
    int    pendingMovePane_ = -1;       // 拖拽目标窗格
    size_t menuTab_ = SIZE_MAX;         // 右键菜单作用的分页
    bool tabDragActive_ = false;        // 分页拖拽进行中
    int  tabDragPane_ = -1;             // 拖拽源窗格
    int  tabDragIndex_ = -1;            // 拖拽源窗格内的 tab 序号
    DWORD lastBlankClickTime_ = 0;      // 分页栏空白区单击时间（双击检测用）
    POINT lastBlankClickPt_ = {0, 0};   // 分页栏空白区单击位置（双击检测用）

    TabState& CurTab() { return tabs_[activeTab_]; }
    const TabState& CurTab() const { return tabs_[activeTab_]; }
    Pane& CurPane() { return panes_[activePane_]; }

    // 窗格/分页管理
    void AddRightTab(bool navigateToDefault = true, size_t paneIdx = SIZE_MAX);
    void AddPane();                     // 新增窗格（含一个分页）
    void SetPaneCount(int n);           // Alt+1~4：窗格数量 1~4
    void SetTriLayout(int t);           // 3 窗格形态：0=品字形 1=倒品字形
    int  AllocPaneTag();                // 分配未被占用的窗格编号（避免控件 ID 重复）
    void RemovePane(size_t idx);        // 销毁窗格并修正 TabState.pane 下标
    int  PaneOfList(HWND h) const;      // 列表 hwnd -> 窗格下标，-1 表示不是窗格列表
    int  PaneOfTab(HWND h) const;       // Tab 容器 hwnd -> 窗格下标
    TabState& PaneActiveTab(size_t pi); // 指定窗格当前显示的分页
    std::wstring PaneItemPath(size_t pi, int item); // 指定窗格某行的完整路径
    void ApplySavedLayout();            // 启动时恢复上次布局（默认倒品字形）
    void ResetAllPanes();               // 销毁全部窗格/分页（启动恢复会话前清空）
    static std::wstring SessionFilePath(); // 会话文件（与 favorites.txt 同目录）
    void SaveSession();                 // 退出时保存全部分页/窗格/历史
    bool RestoreSession();              // 启动时恢复上次会话（无则默认）；返回是否命中会话
    void SelectRightTab(size_t index);
    void SelectPane(size_t index);
    bool PaneHasDir(size_t paneIdx, const std::wstring& dir) const; // 该窗格是否已有同一目录的分页
    void RemoveTab(size_t index, bool& paneEmptied); // 删除分页（paneEmptied=所属窗格变空了吗）
    void CloseRightTab(size_t index);
    void MoveTabToPane(size_t tabIndex, size_t paneIdx); // 分页拖拽移动
    void UpdateRightTabLabels();
    void UpdateNewTabButtons(); // 每个窗格的“+”按钮贴在其最后一个分页头右侧
    void ShowPaneToolsMenu(size_t paneIdx); // 窗格右上角“▾”：外部工具下拉菜单（可配置）
    void RunPaneTool(size_t paneIdx, const std::wstring& name, const std::wstring& cmdline);
    size_t PaneTabPos(size_t paneIdx, size_t tabIndex) const; // 分页在本窗格中的序号
    void TabContextMenu(HWND h, int idx, POINT screenPt);     // 分页标题右键菜单
    void CloseOtherTabs(size_t keepTabIndex);                 // 关闭同一窗格的其它分页
    void CloseRightTabs(size_t keepTabIndex);                 // 关闭同一窗格中它右边的分页
    void ToggleTabLock(size_t tabIndex);                      // 锁定 / 解锁该分页

    size_t pageSize_ = 100;
    // 关闭分页开关时用“一页装下所有”的档位（ PageManager 对它按普通页处理，
    // 分页栏整条隐藏，状态栏显示“未分页”）
    static constexpr size_t kUnlimitedPageSize = 1000000000;
    bool paginationEnabled_ = true;   // 分页开关（设置界面可关）
    size_t EffectivePageSize() const { return paginationEnabled_ ? pageSize_ : kUnlimitedPageSize; }
    int sortCol_ = 0;      // 排序列（全局记住）
    bool sortAsc_ = true;
    bool showGridLines_ = true;  // 文件列表是否画网格线（设置界面可关）
};
