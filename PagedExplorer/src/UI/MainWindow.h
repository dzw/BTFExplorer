#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include "../Pagination/PageManager.h"
#include "../FileModel/FileEntry.h"

// 资源管理器主窗口：树 + 虚拟 ListView + 地址栏 + 分页栏 + 状态栏
class MainWindow {
public:
    static MainWindow* Create(HINSTANCE hInst);

    PageManager& Pages() { return pages_; }

private:
    MainWindow() = default;

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListViewProcStatic(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(UINT, WPARAM, LPARAM);
    LRESULT ListViewProc(UINT, WPARAM, LPARAM);

    // UI 构建
    void BuildChildren();
    void BuildImageList();
    void Layout();
    void CreateSidePanel();          // 左侧 Tab 容器：目录树 / 收藏
    void SwitchSideTab(int index);   // 切换 tab 显示
    void CreateTree();
    void CreateListView();
    void Navigate(const std::wstring& path, bool addHistory = true);

    // 列表
    void RefreshList();               // 重新请求当前页
    void OnPageLoaded();              // 后台加载完成（已 PostMessage 转到 UI 线程）
    void UpdateStatusBar();
    void UpdatePaginationBar();
    void OnColumnClick(int col);
    void ApplyCurrentSort();
    void InsertColumns();
    int  EnsureIcon(FileEntry& e);       // 系统图像列表索引（懒取并缓存）
    bool SelectedPath(std::wstring& out) const;
    std::wstring CurrentPagePath(int item) const; // item -> full path

    // 树
    void PopulateDrives();
    void ExpandTreeNode(HTREEITEM item);

    // 收藏
    void LoadFavorites();
    void SaveFavorites();
    void RefreshFavoritesList();
    void OnAddFavorite();
    void OnRemoveFavorite();

    // 键盘/命令
    void OnDelete();
    void OnRename();
    void OnClipboard(bool cut, bool copyOnly = false);
    void OnPaste();

    // 数据
    HWND hwnd_ = nullptr, tree_ = nullptr, list_ = nullptr, address_ = nullptr;
    HWND tab_ = nullptr, favList_ = nullptr;   // 左侧 Tab 容器 + 收藏列表
    std::vector<std::wstring> favorites_;      // 收藏的目录路径
    HWND status_ = nullptr, pagerPrev_ = nullptr, pagerNext_ = nullptr;
    HWND pagerFirst_ = nullptr, pagerLast_ = nullptr, pagerLabel_ = nullptr, pagerSize_ = nullptr;
    HWND btnBack_ = nullptr, btnFwd_ = nullptr, btnUp_ = nullptr, btnRefresh_ = nullptr;
    WNDPROC listOldProc_ = nullptr;
    HIMAGELIST imgList_ = nullptr;
    HFONT uiFont_ = nullptr;

    std::wstring curDir_;
    std::vector<std::wstring> history_;
    int histPos_ = -1;

    std::vector<FileEntry> pageItems_;   // 当前页条目
    size_t curPage_ = 0;
    size_t pageSize_ = 100;
    int sortCol_ = 0;      // 排序列（作用于当前页）
    bool sortAsc_ = true;

    PageManager pages_;
};
