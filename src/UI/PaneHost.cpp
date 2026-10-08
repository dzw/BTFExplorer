#include "PaneHost.h"

#include <windows.h>
#include <commctrl.h>
#include <vector>

#include <atlbase.h>
#include <atltypes.h>  // ATL 的 CPoint/CSize/CRect（WTL 的 MSG_WM_* 宏按这些类型拆参数）
#include <atlwin.h>    // ATL::CWindowImpl —— WTL 10 不再自带窗口实现，直接复用 ATL 的
#include <atlapp.h>    // WTL 10
#include <atlcrack.h>  // WTL 的消息映射宏（MSG_WM_SIZE 等）

namespace {
constexpr int kEdgePad = 4;        // 内容区与容器左/右/下的留白
constexpr int kStripPad = 6;       // 分页标题行下方的留白
constexpr int kDefaultStripH = 30; // 分页头未量到时的标题行高度（24 行高 + 留白）
}

// 容器窗口本体。分页栏、列表、shell 视图宿主都由主窗口创建后登记进来，
// 本类只负责“标题行 + 内容区”的切分，以及把通知转回主窗口。
struct PaneHost::Impl : public ATL::CWindowImpl<Impl> {
    DECLARE_WND_CLASS_EX(L"PEShellPaneHost", 0, COLOR_WINDOW + 1)

    HWND tab = nullptr;
    HWND list = nullptr;
    std::vector<HWND> contents;
    int stripH = kDefaultStripH;

    BEGIN_MSG_MAP_EX(Impl)
        MSG_WM_SIZE(OnSize)
        // 分页栏/列表的 WM_COMMAND、WM_NOTIFY 原本直接发给主窗口（它们是主窗口的
        // 子窗口）；现在父窗口是容器，得在这里转手，主窗口的处理逻辑不用改。
        MESSAGE_HANDLER(WM_COMMAND, ForwardToParent)
        MESSAGE_HANDLER(WM_NOTIFY, ForwardToParent)
    END_MSG_MAP()

    // ATL 默认在 PostNcDestroy 里 delete this；本对象由 PaneHost 持有，
    // 销毁窗口后还要读一眼状态，所以留给 PaneHost::~PaneHost 来删。
    void PostNcDestroy() {}

    void OnSize(UINT, CSize size)
    {
        PlaceAll(static_cast<int>(size.cx), static_cast<int>(size.cy));
    }

    // ATL 的 MESSAGE_HANDLER 会把 bHandled 传进处理函数
    LRESULT ForwardToParent(UINT msg, WPARAM wp, LPARAM lp, BOOL& handled)
    {
        HWND parent = GetParent();
        if (!parent) {
            handled = FALSE;   // 交回默认处理
            return 0;
        }
        return SendMessageW(parent, msg, wp, lp);
    }

    void PlaceAll(int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        Place(w, h, stripH);
        // 头高只随字体/DPI 变、与容器高度无关：先按上次的值摆，再按实测精修一次。
        // 两者不会互相拉扯，也就不会出现逐次重排越摆越高的抖动。
        RECT rr{};
        if (tab && SendMessageW(tab, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rr))) {
            int measured = (rr.bottom - rr.top) + kStripPad;
            if (measured != stripH && measured > 0) {
                stripH = measured;
                Place(w, h, stripH);
            }
        }
    }

    void Place(int w, int h, int strip)
    {
        // 只挪不画：中间态由主窗口 Layout 末尾的一次 RedrawWindow 统一刷新，
        // 这是“切分页不闪空白”的前提。
        auto place = [](HWND hwnd, int x, int y, int cw, int ch) {
            if (!hwnd) return;
            ::SetWindowPos(hwnd, nullptr, x, y, cw, ch,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
        };
        if (tab) place(tab, 0, 0, w, strip);
        const int contentY = strip;
        const int contentH = h - strip - kEdgePad;
        if (contentH <= 0) return;
        if (list) place(list, kEdgePad, contentY, w - kEdgePad * 2, contentH);
        // shell 视图宿主与列表重叠摆放，可见性由分页的 listMode 决定
        for (HWND hwnd : contents)
            place(hwnd, kEdgePad, contentY, w - kEdgePad * 2, contentH);
    }
};

PaneHost::~PaneHost()
{
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    delete impl_;
    impl_ = nullptr;
}

PaneHost::PaneHost(PaneHost&& other) noexcept : impl_(other.impl_), hwnd_(other.hwnd_)
{
    other.impl_ = nullptr;
    other.hwnd_ = nullptr;
}

PaneHost& PaneHost::operator=(PaneHost&& other) noexcept
{
    if (this != &other) {
        if (hwnd_) DestroyWindow(hwnd_);
        delete impl_;
        impl_ = other.impl_;
        hwnd_ = other.hwnd_;
        other.impl_ = nullptr;
        other.hwnd_ = nullptr;
    }
    return *this;
}

bool PaneHost::Create(HWND parent, int controlId)
{
    if (impl_) return false;
    RECT rc{};
    impl_ = new Impl();
    // ATL::CWindowImpl::Create 的矩形参数是 _U_RECT，绑的是非 const RECT&，
    // 传临时对象编译不过，所以给个具名变量。
    HWND hwnd = impl_->Create(parent, rc, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, controlId);
    if (!hwnd) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    hwnd_ = hwnd;
    return true;
}

void PaneHost::SetTabStrip(HWND tab)
{
    if (!impl_) return;
    impl_->tab = tab;
}

void PaneHost::SetList(HWND list)
{
    if (!impl_) return;
    impl_->list = list;
    Relayout();
}

void PaneHost::AddContent(HWND content)
{
    if (!impl_ || !content) return;
    for (HWND hwnd : impl_->contents)
        if (hwnd == content) return;
    impl_->contents.push_back(content);
    Relayout();   // 新建的宿主尺寸还是 0，不等 WM_SIZE 就得摆一次
}

void PaneHost::RemoveContent(HWND content)
{
    if (!impl_) return;
    for (auto it = impl_->contents.begin(); it != impl_->contents.end(); ++it) {
        if (*it == content) {
            impl_->contents.erase(it);
            return;
        }
    }
}

void PaneHost::Relayout()
{
    if (!impl_ || !hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    impl_->PlaceAll(rc.right - rc.left, rc.bottom - rc.top);
}
