#include "TrayIcon.h"
#include <shellapi.h>
#include <string>
#include "../Util/AppLog.h"

namespace {
constexpr UINT TRAY_ICON_ID = 1;
constexpr UINT MENU_OPEN = 1;
constexpr UINT MENU_SETTINGS = 2;
constexpr UINT MENU_EXIT = 3;
}

bool TrayIcon::Add(HWND owner, HICON icon)
{
    WriteAppLog(L"TRAY_ICON add requested");
    owner_ = owner;
    icon_ = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    visible_ = true;
    if (AddToShell()) {
        WriteAppLog(L"TRAY_ICON added");
        return true;
    }
    visible_ = false;
    return false;
}

void TrayIcon::Remove()
{
    visible_ = false;
    if (!registered_) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &data);
    registered_ = false;
}

bool TrayIcon::RestoreAfterTaskbarRestart()
{
    if (!visible_) return true;
    registered_ = false;
    return AddToShell();
}

bool TrayIcon::AddToShell()
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = TRAY_ICON_ID;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = CallbackMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"PagedExplorer");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        std::wstring message = L"TRAY_ICON Shell_NotifyIcon(NIM_ADD) failed, error=" +
                               std::to_wstring(GetLastError());
        WriteAppLog(message.c_str());
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    registered_ = true;
    return true;
}

TrayAction TrayIcon::ShowContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        MessageBoxW(owner_, L"无法创建托盘菜单。", L"PagedExplorer", MB_OK | MB_ICONERROR);
        return TrayAction::None;
    }

    bool added =
        AppendMenuW(menu, MF_STRING, MENU_OPEN, L"打开应用") &&
        AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"设置") &&
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(menu, MF_STRING, MENU_EXIT, L"退出");
    if (!added) {
        DestroyMenu(menu);
        MessageBoxW(owner_, L"无法创建托盘菜单项。", L"PagedExplorer", MB_OK | MB_ICONERROR);
        return TrayAction::None;
    }

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(owner_);
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  point.x, point.y, 0, owner_, nullptr);
    DestroyMenu(menu);
    PostMessageW(owner_, WM_NULL, 0, 0);

    switch (command) {
    case MENU_OPEN: return TrayAction::Open;
    case MENU_SETTINGS: return TrayAction::Settings;
    case MENU_EXIT: return TrayAction::Exit;
    default: return TrayAction::None;
    }
}

TrayAction TrayIcon::HandleCallback(LPARAM event)
{
    std::wstring message = L"TRAY_ICON callback event=" +
                           std::to_wstring(static_cast<unsigned long>(LOWORD(event)));
    WriteAppLog(message.c_str());
    switch (LOWORD(event)) {
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        return ShowContextMenu();
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        return TrayAction::Open;
    default:
        return TrayAction::None;
    }
}
