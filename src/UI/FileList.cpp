#include "FileList.h"
#include "../FileModel/FileEntry.h"
#include "../Shell/ShellUtil.h"
#include "../Util/AppLog.h"
#include <shellapi.h>
#include <windowsx.h>
#include <cwchar>

// 慢双击重命名：第二次单击超过系统双击时间、但仍在该时间窗内才算“慢双击”
static constexpr DWORD kSlowRenameWindow = 1500; // ms

namespace filelist {

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

// 系统图像列表只取一次，所有窗格列表共用（LVS_SHAREIMAGELISTS）
HIMAGELIST SysImageList()
{
    static HIMAGELIST sys = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, nullptr, 0, SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    return sys;
}

} // namespace filelist

// 列表现在是主窗口的直接子窗口（不再随 tab 控件一起销毁），关窗格时要显式销毁
FileList::~FileList()
{
    if (list_) DestroyWindow(list_);
    list_ = nullptr;
}

bool FileList::Create(HWND parent, FileListDelegate* delegate, HFONT font, int controlId)
{
    delegate_ = delegate;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SHAREIMAGELISTS,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), hInst, nullptr);
    if (!list_) return false;
    if (font) SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    // 整行选中 + 双缓冲（双缓冲是拖分隔条不闪的前提）；网格线受设置开关控制
    DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
    if (gridLines_) ex |= LVS_EX_GRIDLINES;
    ListView_SetExtendedListViewStyle(list_, ex);
    if (HIMAGELIST imgs = filelist::SysImageList())
        ListView_SetImageList(list_, imgs, LVSIL_SMALL);

    struct Column { const wchar_t* title; int width; };
    const Column cols[] = { { L"名称", 300 }, { L"类型", 140 }, { L"大小", 110 }, { L"修改日期", 160 } };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, LVCFMT_LEFT, cols[i].width,
                        const_cast<LPWSTR>(cols[i].title) };
        c.iSubItem = i;
        ListView_InsertColumn(list_, i, &c);
    }

    // 子类化：GWLP_USERDATA 挂本对象；原过程必须保存，消息链尾要交还控件自己
    SetWindowLongPtrW(list_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    origProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(list_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&FileList::ProcStatic)));
    return true;
}

void FileList::SetGridLines(bool show)
{
    gridLines_ = show;
    if (!list_) return;
    DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
    if (show) ex |= LVS_EX_GRIDLINES;
    ListView_SetExtendedListViewStyle(list_, ex);
}

// 列表自己的消息过程：分隔条命中、慢双击重命名、窗格激活延后、键盘命令、右键菜单。
// 业务动作全部经 FileListDelegate 交回主窗口。
LRESULT FileList::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONDOWN || msg == WM_SETCURSOR) {
        // 上下两排窗格之间的水平分隔条：命中区比可见留白宽，从列表顶部伸进来
        POINT pt{};
        if (msg == WM_LBUTTONDOWN) {
            pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &pt);
        } else {
            GetCursorPos(&pt);
        }
        ScreenToClient(GetAncestor(h, GA_ROOT), &pt);
        if (delegate_ && delegate_->HitRowSplit(h, pt)) {
            if (msg == WM_LBUTTONDOWN) {
                delegate_->RowSplitClick(h, wp, pt);
            } else {
                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            }
            return msg == WM_SETCURSOR ? TRUE : 0;
        }
    }

    // 慢双击（资源管理器习惯）：单击选中、停顿一下再单击同一项 -> 重命名。
    // 快速双击走系统双击 -> NM_DBLCLK 打开。
    if (msg == WM_LBUTTONDOWN) {
        LVHITTESTINFO ht{};
        ht.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = ListView_HitTest(h, &ht);
        DWORD now = GetMessageTime();
        DWORD delta = (DWORD)(now - lastClickTime_);
        bool modifier = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000);
        if (modifier) {
            lastClickItem_ = -1;      // Ctrl/Shift 是选择修饰键，不参与慢双击
            lastClickTime_ = 0;
        } else if (idx >= 0 && idx == lastClickItem_ && ListView_GetItemState(h, idx, LVIS_SELECTED) &&
                   delta >= (DWORD)GetDoubleClickTime() && delta <= kSlowRenameWindow) {
            lastClickItem_ = -1;      // 复位，避免连点连续触发
            lastClickTime_ = 0;
            if (delegate_) delegate_->SlowRename(h, idx);
            return 0;                 // 吃掉这次点击（项已选中，无需再改选择）
        } else {
            lastClickItem_ = idx;     // 空白处 idx<0 也记录，会覆盖上一项
            lastClickTime_ = now;
        }
    }

    if (delegate_) {
        // 只有真正的用户输入才激活窗格；悬停/滚动等一律不切
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
            msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
            delegate_->ActivatePaneDeferred(h);

        if (msg == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && wp == 'C')      { delegate_->ListKeyCommand(h, CmdCopy); return 0; }
            if (ctrl && wp == 'X')      { delegate_->ListKeyCommand(h, CmdCut); return 0; }
            if (ctrl && wp == 'V')      { delegate_->ListKeyCommand(h, CmdPaste); return 0; }
            if (wp == VK_DELETE)        { delegate_->ListKeyCommand(h, shift ? CmdDeleteNoRecycle : CmdDelete); return 0; }
            if (wp == VK_ESCAPE)        { delegate_->ListKeyCommand(h, CmdEscape); return 0; }
            // F2 不拦截：放行给默认处理，走 LVN_KEYDOWN 的重命名通知
        }

        if (msg == WM_CONTEXTMENU) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool keyboard = pt.x == -1 && pt.y == -1;
            if (keyboard) {
                pt = { 200, 200 };
                ClientToScreen(h, &pt);
            }
            delegate_->ListContextMenu(h, pt, keyboard);
            return 0;
        }
    }

    WNDPROC orig = origProc_;
    return orig ? CallWindowProcW(orig, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK FileList::ProcStatic(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR)
{
    auto* self = reinterpret_cast<FileList*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

// 主窗口把窗格列表的 WM_NOTIFY 转进来；显示数据经回调取，交互动作经回调交回
bool FileList::HandleNotify(NMHDR* nm)
{
    if (!delegate_) return false;
    HWND h = nm->hwndFrom;
    switch (nm->code) {
    case NM_CLICK:
    case NM_DBLCLK:
    case NM_RCLICK:
    case NM_RDBLCLK:
    case NM_RETURN:
    case LVN_COLUMNCLICK:
    case LVN_KEYDOWN:
        // 这些通知是列表控件在自己消息过程里同步发来的，激活窗格会重入列表
        // （RefreshList -> LVM_SETITEMCOUNT），由宿主延后执行
        delegate_->ActivatePaneDeferred(h);
        break;
    }

    if (nm->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        int i = di->item.iItem;
        if (i < 0) return true;
        if (di->item.mask & LVIF_TEXT) {
            std::wstring text;
            if (delegate_->GetItemText(h, i, di->item.iSubItem, text))
                wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
        }
        if (di->item.mask & LVIF_IMAGE)
            di->item.iImage = delegate_->GetItemIcon(h, i);
        return true;
    }
    if (nm->code == NM_DBLCLK) {
        auto* ni = reinterpret_cast<NMITEMACTIVATE*>(nm);
        if (ni->iItem >= 0) delegate_->ListItemActivated(h, ni->iItem);
        return true;
    }
    if (nm->code == LVN_COLUMNCLICK) {
        auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
        delegate_->ListColumnClicked(h, lv->iSubItem);
        return true;
    }
    if (nm->code == LVN_KEYDOWN) {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
        if (kd->wVKey == VK_F2) delegate_->ListKeyCommand(h, CmdRename);
        return true;
    }
    return false;
}
