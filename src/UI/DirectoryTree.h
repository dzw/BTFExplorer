#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>

class DirectoryTree {
public:
    HWND Create(HWND parent, int controlId, HFONT font);
    HWND Handle() const { return hwnd_; }

    void PopulateDrives();
    bool HandleNotification(const NMHDR* notification, std::wstring& selectedPath);
    void SyncToPath(const std::wstring& path, bool showErrors);

private:
    void ExpandNode(HTREEITEM item);

    HWND hwnd_ = nullptr;
    bool syncingSelection_ = false;
};
