#include "PaneHost.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include "../Util/AppLog.h"

#include <atlbase.h>
#include <atltypes.h>  // ATL 的 CPoint/CSize/CRect（WTL 的 MSG_WM_* 宏按这些类型拆参数）
#include <atlwin.h>    // ATL::CWindowImpl —— WTL 10 不再自带窗口实现，直接复用 ATL 的
#include <atlapp.h>    // WTL 10
#include <atlcrack.h>  // WTL 的消息映射宏（MSG_WM_SIZE 等）

namespace {
constexpr int kEdgePad = 0;        // 内容区（列表 / shell 视图）齐边贴满窗格，不留缝隙
constexpr int kStripPad = 6;       // 分页标题行下方的留白
constexpr int kDefaultStripH = 30; // 分页头未量到时的标题行高度（24 行高 + 留白）
constexpr wchar_t kBlankHitClass[] = L"PEShellTabBlankHit";
// 独立地址栏行左侧的导航按钮带（Q-Dir 同款：←/→/↑/⌂ 挤在地址栏左边）
constexpr int kNavBtnW = 28;  // 单个导航按钮宽
constexpr int kNavGap = 2;    // 按钮间距
constexpr int kNavPad = 4;    // 带左侧留白（也是带与地址栏之间的缝）
constexpr int kNavMax = 4;    // 最多 4 个（←/→/↑/⌂）
}

// 容器窗口本体。分页栏、列表、shell 视图宿主都由主窗口创建后登记进来，
// 本类只负责“标题行 + 内容区”的切分，以及把通知转回主窗口。
struct PaneHost::Impl : public ATL::CWindowImpl<Impl> {
    DECLARE_WND_CLASS_EX(L"PEShellPaneHost", 0, COLOR_WINDOW + 1)

    HWND tab = nullptr;
    HWND addressBar = nullptr;  // 顶部地址栏行（每窗格独立地址栏，主窗口创建后登记）
    int addressH = 0;           // 地址栏行高（0 = 不启用，不占行）
    HWND navBtns[kNavMax] = {}; // 地址栏行左侧的导航按钮（←/→/↑/⌂，显隐由主窗口控制）
    int navCount = 0;
    HWND blankHit = nullptr;
    HWND list = nullptr;
    std::vector<HWND> contents;
    int stripH = kDefaultStripH;
    int tabTopPad_ = 0;   // 标题行上方的留白（侧栏用它和顶部工具栏拉开距离）
    int tabRightReserve_ = 0;  // 标题行右侧留宽（给 ▾/Q/0 等按钮让位，按钮移到条外）
    HWND bottomPane_ = nullptr; // 底部窗格（固定高贴底，如分页栏）
    int bottomH_ = 0;           // 底部窗格高度

    BEGIN_MSG_MAP_EX(Impl)
        MSG_WM_SIZE(OnSize)
        // 内缩留白(Border)不刷会露出后面的深色，这里统一刷成列表同款底色
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        // 分页栏/列表的 WM_COMMAND、WM_NOTIFY 原本直接发给主窗口（它们是主窗口的
        // 子窗口）；现在父窗口是容器，得在这里转手，主窗口的处理逻辑不用改。
        MESSAGE_HANDLER(WM_COMMAND, ForwardToParent)
        MESSAGE_HANDLER(WM_NOTIFY, ForwardToParent)
    END_MSG_MAP()

    static LRESULT CALLBACK BlankHitProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return ::DefWindowProcW(hwnd, msg, wp, lp);

        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            WriteAppLog((L"TAB_BLANK_HIT event=" + std::to_wstring(msg) +
                         L" client=" + std::to_wstring(pt.x) + L"," +
                         std::to_wstring(pt.y)).c_str());
            if (msg == WM_LBUTTONDBLCLK) {
                HWND main = GetAncestor(hwnd, GA_ROOT);
                if (!main || !::PostMessageW(
                        main, WM_APP_PANE_TAB_BLANK_DBLCLK,
                        reinterpret_cast<WPARAM>(self->tab), 0)) {
                    WriteAppLog((L"TAB_BLANK_HIT post failed err=" +
                                 std::to_wstring(GetLastError())).c_str());
                }
            }
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ATL 默认在 PostNcDestroy 里 delete this；本对象由 PaneHost 持有，
    // 销毁窗口后还要读一眼状态，所以留给 PaneHost::~PaneHost 来删。
    void PostNcDestroy() {}

    void OnSize(UINT, CSize size)
    {
        PlaceAll(static_cast<int>(size.cx), static_cast<int>(size.cy));
    }

    // 容器背景统一刷成列表同款底色（内容区现已齐边贴满，无留白，这里只是兜底底色）
    LRESULT OnEraseBkgnd(UINT /*msg*/, WPARAM wparam, LPARAM /*lparam*/, BOOL& handled)
    {
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT rc{};
        GetClientRect(&rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        handled = TRUE;
        return 1;
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
        int tabW = (w > tabRightReserve_) ? (w - tabRightReserve_) : w;
        // 顶部地址栏行（每窗格独立地址栏）：压在最上，分页栏与内容区整体下移让位。
        // 导航按钮带（←/→/↑/⌂）挤在地址栏左边，地址栏只占剩下的宽度。
        const int addrH = (addressBar != nullptr && addressH > 0) ? addressH : 0;
        int navW = 0;
        for (int i = 0; i < navCount && i < kNavMax; ++i) {
            if (navBtns[i] == nullptr) continue;
            place(navBtns[i], kNavPad + i * (kNavBtnW + kNavGap), 0, kNavBtnW, addrH);
            navW = kNavPad + (i + 1) * (kNavBtnW + kNavGap);
        }
        if (addressBar) place(addressBar, navW, 0, (w > navW) ? (w - navW) : 0, addrH);
        if (tab) place(tab, 0, addrH + tabTopPad_, tabW, strip);
        const int contentY = addrH + tabTopPad_ + strip;
        const int contentH = h - contentY - bottomH_;
        if (contentH > 0) {
            if (list) place(list, kEdgePad, contentY, w - kEdgePad * 2, contentH);
            // shell 视图宿主与列表重叠摆放，可见性由分页的 listMode 决定
            for (HWND hwnd : contents)
                place(hwnd, kEdgePad, contentY, w - kEdgePad * 2, contentH);
        }
        // 底部窗格（如分页栏）：固定高度贴在容器底部，上面的内容区相应变矮
        if (bottomPane_ && bottomH_ > 0)
            place(bottomPane_, 0, h - bottomH_, w, bottomH_);
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

void PaneHost::SetTabTopPad(int pad)
{
    if (!impl_) return;
    impl_->tabTopPad_ = pad;
    Relayout();   // 改了留白立即按新尺寸重摆
}

void PaneHost::SetTabRightReserve(int reserve)
{
    if (!impl_) return;
    impl_->tabRightReserve_ = reserve;
    Relayout();   // 改了留宽立即按新尺寸重摆
}

void PaneHost::SetBottomPane(HWND pane, int height)
{
    if (!impl_) return;
    impl_->bottomPane_ = pane;
    impl_->bottomH_ = height;
    Relayout();
}

void PaneHost::SetAddressBar(HWND edit, int height)
{
    if (!impl_) return;
    impl_->addressBar = edit;
    impl_->addressH = height;
    Relayout();   // 行高变了立即重摆（显示/隐藏由主窗口控制）
}

void PaneHost::SetNavButtons(const HWND* btns, int count)
{
    if (!impl_) return;
    impl_->navCount = (count > kNavMax) ? kNavMax : count;
    for (int i = 0; i < impl_->navCount; ++i)
        impl_->navBtns[i] = btns[i];
    for (int i = impl_->navCount; i < kNavMax; ++i)
        impl_->navBtns[i] = nullptr;
    Relayout();
}

void PaneHost::SetTabStrip(HWND tab)
{
    if (!impl_) return;
    impl_->tab = tab;
    if (impl_->blankHit) SetWindowPos(impl_->blankHit, HWND_TOP, 0, 0, 0, 0,
                                      SWP_NOACTIVATE | SWP_HIDEWINDOW);
    if (!tab) return;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(instance, kBlankHitClass, &wc)) {
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Impl::BlankHitProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBlankHitClass;
        if (!RegisterClassExW(&wc)) {
            WriteAppLog((L"TAB_BLANK_HIT class registration failed err=" +
                         std::to_wstring(GetLastError())).c_str());
            return;
        }
    }
    impl_->blankHit = CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kBlankHitClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, hwnd_, nullptr, instance, impl_);
    if (!impl_->blankHit)
        WriteAppLog((L"TAB_BLANK_HIT window creation failed err=" +
                     std::to_wstring(GetLastError())).c_str());
}

void PaneHost::SetTabBlankHitArea(const RECT& area)
{
    if (!impl_ || !impl_->blankHit) return;
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width <= 0 || height <= 0) {
        ShowWindow(impl_->blankHit, SW_HIDE);
        return;
    }
    SetWindowPos(impl_->blankHit, HWND_TOP,
                 area.left, area.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
