#include "SettingsDialog.h"
#include <commctrl.h>
#include <windowsx.h>   // Button_GetCheck / Button_SetCheck 宏在这里，不在 commctrl.h
#include <cstdio>       // swprintfW
#include <string>
#include <vector>

#pragma comment(lib, "Advapi32.lib")

namespace {

// 对话框内部的控件 ID（只在本文件用，不进主窗口的公共枚举）
enum {
    IDC_OPT_GRID = 1040, IDC_OPT_PANES1 = 1041, IDC_OPT_PANES2 = 1042,
    IDC_OPT_PANES3 = 1043, IDC_OPT_PANES4 = 1044,
    IDC_OPT_TRI_TOP = 1045, IDC_OPT_TRI_DOWN = 1046,
    IDC_OPT_PAGING = 1047, IDC_OPT_STARTUP = 1048,
    IDC_OPT_APPLY = 1049,   // “应用”按钮（即时生效，对话框不关闭）
};

constexpr wchar_t kDlgClass[] = L"PagedExplorerSettingsBox";
constexpr wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[]  = L"PagedExplorer";

// 对话框尺寸（高度要盖住非客户区：标题栏 + 边框约占 30px）
constexpr int kDlgW = 404, kDlgH = 330;

// 模态期间共享的状态（同一时刻只有一个设置对话框）
struct State {
    SettingsData data{};   // 进对话框时的初值 / 按确定后的结果
    bool applied = false;  // 用户是否按了“确定”
    HFONT font = nullptr;  // 主窗口的界面字体
    std::function<void(const SettingsData&)> apply; // “确定/应用”时即时套用
};
State g;

// 一组连续 radio 里当前选中的是第几个（都没选时返回 0）
int RadioChecked(HWND parent, int firstId, int count)
{
    for (int i = 0; i < count; ++i)
        if (Button_GetCheck(GetDlgItem(parent, firstId + i)) == BST_CHECKED)
            return i;
    return 0;
}

// 勾选状态要放在 **wParam**（即用 Button_SetCheck）。BM_SETCHECK 的 MSDN 措辞写成
// lParam，但按 lParam 传会被静默忽略，表现为“设了没反应”。
void SetCheck(HWND parent, int id, bool on)
{
    Button_SetCheck(GetDlgItem(parent, id), on ? BST_CHECKED : BST_UNCHECKED);
}

// 只有三窗格时“排列方式”才有意义
void UpdateTriEnabled(HWND dlg)
{
    bool on = (RadioChecked(dlg, IDC_OPT_PANES1, 4) + 1) == 3;
    EnableWindow(GetDlgItem(dlg, IDC_OPT_TRI_TOP), on);
    EnableWindow(GetDlgItem(dlg, IDC_OPT_TRI_DOWN), on);
}

void ApplyFont(HWND h)
{
    if (g.font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
}

// 把当前控件状态读回 g.data
void ReadControls(HWND h)
{
    g.data.gridLines  = (Button_GetCheck(GetDlgItem(h, IDC_OPT_GRID)) == BST_CHECKED);
    g.data.pagination = (Button_GetCheck(GetDlgItem(h, IDC_OPT_PAGING)) == BST_CHECKED);
    g.data.paneCount  = RadioChecked(h, IDC_OPT_PANES1, 4) + 1;
    g.data.triLayout  = RadioChecked(h, IDC_OPT_TRI_TOP, 2);
    g.data.autoStart  = (Button_GetCheck(GetDlgItem(h, IDC_OPT_STARTUP)) == BST_CHECKED);
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK || id == IDCANCEL) {
            if (id == IDOK) {
                ReadControls(h);
                if (g.apply) g.apply(g.data);   // 即时套用
                g.applied = true;
            }
            DestroyWindow(h);
            return 0;
        }
        if (id == IDC_OPT_APPLY) {
            ReadControls(h);
            if (g.apply) g.apply(g.data);       // 即时套用，对话框保持打开
            return 0;
        }
        if (id >= IDC_OPT_PANES1 && id <= IDC_OPT_PANES4) { UpdateTriEnabled(h); return 0; }
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

namespace settings {

bool GetAutoStart(bool& enabled)
{
    enabled = false;
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) {
        wchar_t message[160];
        swprintf_s(message, L"读取 Windows 启动设置失败（错误码 %ld）。", status);
        MessageBoxW(nullptr, message, L"PagedExplorer", MB_OK | MB_ICONERROR);
        return false;
    }

    DWORD type = 0;
    status = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) {
        wchar_t message[160];
        swprintf_s(message, L"读取 Windows 启动设置失败（错误码 %ld）。", status);
        MessageBoxW(nullptr, message, L"PagedExplorer", MB_OK | MB_ICONERROR);
        return false;
    }
    enabled = type == REG_SZ || type == REG_EXPAND_SZ;
    return true;
}

void SetAutoStart(bool enabled)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                     KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        wchar_t message[160];
        swprintf_s(message, L"打开 Windows 启动设置失败（错误码 %ld）。", status);
        MessageBoxW(nullptr, message, L"PagedExplorer", MB_OK | MB_ICONERROR);
        return;
    }

    if (enabled) {
        std::vector<wchar_t> path(512);
        DWORD length = 0;
        for (;;) {
            length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0) { status = GetLastError(); break; }
            if (length < path.size() - 1) {
                std::wstring command = L"\"" + std::wstring(path.data(), length) + L"\"";
                status = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
                break;
            }
            if (path.size() >= 32768) { status = ERROR_INSUFFICIENT_BUFFER; break; }
            path.resize(path.size() * 2);
        }
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        wchar_t message[160];
        swprintf_s(message, L"保存 Windows 启动设置失败（错误码 %ld）。", status);
        MessageBoxW(nullptr, message, L"PagedExplorer", MB_OK | MB_ICONERROR);
    }
}

bool Show(HWND owner, HFONT font, SettingsData& data,
           std::function<void(const SettingsData&)> onApply)
{
    g.data = data;
    g.applied = false;
    g.font = font;
    g.apply = std::move(onApply);

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    static ATOM cls = 0;
    if (!cls) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kDlgClass;
        cls = RegisterClassW(&wc);
    }

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kDlgClass, L"设置",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, kDlgW, kDlgH, owner, nullptr, hInst, nullptr);
    if (!dlg) { g.font = nullptr; return false; }
    RECT rm; GetWindowRect(owner, &rm);
    SetWindowPos(dlg, nullptr,
        (rm.left + rm.right) / 2 - kDlgW / 2, (rm.top + rm.bottom) / 2 - kDlgH / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    auto lab = [&](const wchar_t* t, int x, int y, int w) {
        HWND h = CreateWindowExW(0, WC_STATICW, t, WS_CHILD | WS_VISIBLE, x, y, w, 20,
                                 dlg, nullptr, hInst, nullptr);
        ApplyFont(h);
        return h;
    };
    auto chk = [&](const wchar_t* t, int id, int x, int y) {
        HWND h = CreateWindowExW(0, WC_BUTTONW, t,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX,
            x, y, 300, 22, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        ApplyFont(h);
        return h;
    };
    auto rad = [&](const wchar_t* t, int id, int x, int y, bool groupStart, int w = 130) {
        DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
        if (groupStart) st |= WS_GROUP;
        HWND h = CreateWindowExW(0, WC_BUTTONW, t, st, x, y, w, 22, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        ApplyFont(h);
        return h;
    };

    // ---- 列表 ----
    lab(L"列表", 20, 12, 200);
    HWND cGrid = chk(L"显示网格线（关闭后列表不再画横竖线）", IDC_OPT_GRID, 34, 34);
    Button_SetCheck(cGrid, g.data.gridLines ? BST_CHECKED : BST_UNCHECKED);

    HWND cPaging = chk(L"启用分页（关闭后一次显示全部条目）", IDC_OPT_PAGING, 34, 58);
    Button_SetCheck(cPaging, g.data.pagination ? BST_CHECKED : BST_UNCHECKED);

    // ---- 窗格布局 ----
    lab(L"窗格布局", 20, 92, 200);
    lab(L"窗格数量：", 34, 120, 80);
    // 单选钮宽度必须收紧（文本只有一位数字，36px 足够）：以前固定 130px 而间距只有
    // 42px，控件矩形互相叠压，z 序在上面的会吃掉下面的大部分点击区域
    for (int i = 0; i < 4; ++i) {
        wchar_t t[8]; swprintf_s(t, L"%d", i + 1);
        rad(t, IDC_OPT_PANES1 + i, 118 + i * 40, 118, i == 0, 36);
        SetCheck(dlg, IDC_OPT_PANES1 + i, g.data.paneCount == i + 1);
    }
    lab(L"三窗格排列：", 34, 150, 90);
    // 两个选项文字较长（约 150px），横排放不下会互相裁剪，改为竖排
    rad(L"品字形（1 上 2 下）", IDC_OPT_TRI_TOP, 128, 148, true, 190);
    rad(L"倒品字形（2 上 1 下）", IDC_OPT_TRI_DOWN, 128, 172, false, 190);
    SetCheck(dlg, IDC_OPT_TRI_TOP, g.data.triLayout == 0);
    SetCheck(dlg, IDC_OPT_TRI_DOWN, g.data.triLayout != 0);

    // ---- 启动 ----
    lab(L"启动", 20, 206, 200);
    HWND cStart = chk(L"Windows 启动时运行本应用", IDC_OPT_STARTUP, 34, 228);
    Button_SetCheck(cStart, g.data.autoStart ? BST_CHECKED : BST_UNCHECKED);

    // 按钮贴客户区底部：以前用 kDlgH-40 定位，没扣掉标题栏/边框高度，
    // 三个按钮下半截被客户区底边裁掉
    RECT rcDlg{}; GetClientRect(dlg, &rcDlg);
    int btnY = rcDlg.bottom - 36;
    HWND ok = CreateWindowExW(0, WC_BUTTONW, L"确定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON,
        214, btnY, 84, 26, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
    CreateWindowExW(0, WC_BUTTONW, L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        306, btnY, 84, 26, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst, nullptr);
    CreateWindowExW(0, WC_BUTTONW, L"应用",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        122, btnY, 84, 26, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPT_APPLY)), hInst, nullptr);
    ApplyFont(ok);
    SendMessageW(dlg, DM_SETDEFID, IDOK, 0);
    UpdateTriEnabled(dlg);

    ShowWindow(dlg, SW_SHOW);
    EnableWindow(owner, FALSE); // 模态

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
    EnableWindow(owner, TRUE);
    g.font = nullptr;

    if (!g.applied) return false;   // 取消：不动调用方的数据
    data = g.data;
    return true;
}

} // namespace settings
