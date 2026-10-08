#pragma once
#include <windows.h>

inline constexpr UINT WM_APP_PANE_TAB_BLANK_DBLCLK = WM_APP + 19;

// Q-Dir 的窗格是三层结构：容器窗口（Spy++ 里的 ATL::C3000）下面挂“只占标题行的
// 分页栏”和“内容视图（SHELLDLL_DefView）”。本类就是那个容器：主窗口只按窗格矩形
// 摆放容器，容器自己在 WM_SIZE 里把分页栏压成一行高、把内容区填满剩余空间。
//
// 实现见 PaneHost.cpp（WTL/ATL 的 CWindowImpl 派生类）。头文件刻意不暴露 ATL 类型，
// 免得每个包含 MainWindow.h 的翻译单元都被拖进 ATL 头。
class PaneHost {
public:
    PaneHost() = default;
    ~PaneHost();
    PaneHost(PaneHost&& other) noexcept;
    PaneHost& operator=(PaneHost&& other) noexcept;
    PaneHost(const PaneHost&) = delete;
    PaneHost& operator=(const PaneHost&) = delete;

    bool Create(HWND parent, int controlId);
    HWND Handle() const { return hwnd_; }

    // 内部控件由主窗口创建并持有生命周期，这里只登记“归容器摆放”
    void SetTabStrip(HWND tab);        // 分页栏（只占标题行）
    void SetTabBlankHitArea(const RECT& area); // 分页栏尾部空白的透明鼠标接收区
    void SetList(HWND list);           // 自绘虚拟列表（内容区）
    void AddContent(HWND content);     // 与列表重叠摆放的其它内容：分页的 shell 视图宿主
    void RemoveContent(HWND content);  // 分页被拖到别的窗格时解除登记（不销毁窗口）
    // 按容器当前尺寸重摆内部控件。新建/迁移的内容窗口尺寸还是 0，
    // 而 SetWindowPos 只在尺寸变化时才发 WM_SIZE，所以摆放容器后要显式调一次。
    void Relayout();

private:
    struct Impl;      // WTL 窗口对象（堆上，地址必须稳定：ATL 按 HWND 反查它）
    Impl* impl_ = nullptr;
    HWND hwnd_ = nullptr;
};
