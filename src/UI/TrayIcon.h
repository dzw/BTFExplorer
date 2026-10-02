#pragma once
#include <windows.h>

enum class TrayAction {
    None,
    Open,
    Settings,
    Exit,
};

class TrayIcon {
public:
    static constexpr UINT CallbackMessage = WM_APP + 5;

    bool Add(HWND owner, HICON icon);
    void Remove();
    bool Visible() const { return visible_; }   // 图标是否已在通知区
    bool RestoreAfterTaskbarRestart();
    TrayAction HandleCallback(LPARAM event);

private:
    bool AddToShell();
    TrayAction ShowContextMenu();

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool visible_ = false;
    bool registered_ = false;
};
