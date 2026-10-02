#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <memory>
#include <vector>
#include "../Pagination/PageManager.h"
#include "../FileModel/FileEntry.h"

// 控件 ID（主窗口与 main.cpp 的加速键 / 单实例逻辑共享）
enum {
    IDC_ADDRESS = 1001, IDC_TREE = 1002, IDC_LIST = 1003,
    IDC_BACK = 1004, IDC_FORWARD = 1005, IDC_UP = 1006, IDC_REFRESH = 1007,
    IDC_FIRST = 1010, IDC_PREV = 1011, IDC_NEXT = 1012, IDC_LAST = 1013,
    IDC_PAGE_SIZE = 1014, IDC_PAGER_LABEL = 1015,
    IDC_TAB = 1016, IDC_FAVLIST = 1017, IDC_RIGHTTAB = 1018, IDC_NEWTAB = 1019,
    IDC_LAYOUT1 = 1020, IDC_LAYOUT2 = 1021, IDC_LAYOUT3 = 1022, IDC_LAYOUT4 = 1023,
    IDC_TRI_PINTOP = 1024, IDC_TRI_PINDOWN = 1025,
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
    void CreateTree();
    void CreatePane(Pane& p);        // 创建一个窗格（tab 容器 + 虚拟列表）
    void Navigate(const std::wstring& path, bool addHistory = true);

    // 列表（作用于激活窗格的当前分页）
    HWND  CurList() const;            // 激活窗格的 ListView
    void RefreshList();               // 重新请求当前页
    void OnPageLoaded();              // 后台加载完成（已 PostMessage 转到 UI 线程）
    void UpdateStatusBar();
    void UpdatePaginationBar();
    void OnColumnClick(int col);
    void ApplyCurrentSort();
    void InsertColumns(HWND list);   // 给指定列表建列
    int  EnsureIcon(FileEntry& e);       // 系统图像列表索引（懒取并缓存）
    bool SelectedPath(std::wstring& out) const;
    std::wstring CurrentPagePath(int item) const; // item -> full path

    // 树
    void PopulateDrives();
    void ExpandTreeNode(HTREEITEM item);

    // 收藏
    void LoadFavorites();
    void SaveFavorites();          // 保存（带防丢失保护：内存为空时保住磁盘收藏）
    void SaveFavoritesCore(bool allowEmptyFavorites); // 显式删除最后一条时传 true
    bool ReadFavoritesFromDisk(std::vector<std::wstring>& out); // 整文件读入，解析成功才返回 true
    void RefreshFavoritesList();
    void OnAddFavorite();
    void OnRemoveFavorite();

    // 键盘/命令
    void OnDelete();
    void OnRename();
    void OnClipboard(bool cut, bool copyOnly = false);
    void OnPaste();

    // 数据
    HWND hwnd_ = nullptr, tree_ = nullptr, address_ = nullptr;
    HWND tab_ = nullptr, favList_ = nullptr;   // 左侧 Tab 容器 + 收藏列表
    HWND btnNewTab_ = nullptr;                 // 激活窗格的 “+” 新增分页按钮
    std::vector<std::wstring> favorites_;      // 收藏的目录路径
    HWND status_ = nullptr, pagerPrev_ = nullptr, pagerNext_ = nullptr;
    HWND pagerFirst_ = nullptr, pagerLast_ = nullptr, pagerLabel_ = nullptr, pagerSize_ = nullptr;
    HWND btnBack_ = nullptr, btnFwd_ = nullptr, btnUp_ = nullptr, btnRefresh_ = nullptr;
    HIMAGELIST imgList_ = nullptr;   // 取不到系统图像列表时的自建兜底
    HIMAGELIST sysImgs_ = nullptr;   // 系统图像列表（所有窗格共用）
    HFONT uiFont_ = nullptr;

    // 布局 / 交互
    int sideWidth_ = 220;          // 左侧面板宽度（可被分隔条拖动改变）
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
    void SelectRightTab(size_t index);
    void SelectPane(size_t index);
    bool PaneHasDir(size_t paneIdx, const std::wstring& dir) const; // 该窗格是否已有同一目录的分页
    void RemoveTab(size_t index, bool& paneEmptied); // 删除分页（paneEmptied=所属窗格变空了吗）
    void CloseRightTab(size_t index);
    void MoveTabToPane(size_t tabIndex, size_t paneIdx); // 分页拖拽移动
    void UpdateRightTabLabels();
    size_t PaneTabPos(size_t paneIdx, size_t tabIndex) const; // 分页在本窗格中的序号
    void TabContextMenu(HWND h, int idx, POINT screenPt);     // 分页标题右键菜单
    void CloseOtherTabs(size_t keepTabIndex);                 // 关闭同一窗格的其它分页
    void CloseRightTabs(size_t keepTabIndex);                 // 关闭同一窗格中它右边的分页
    void ToggleTabLock(size_t tabIndex);                      // 锁定 / 解锁该分页

    size_t pageSize_ = 100;
    int sortCol_ = 0;      // 排序列（全局记住）
    bool sortAsc_ = true;
};
