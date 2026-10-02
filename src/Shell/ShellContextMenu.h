#pragma once
#include <windows.h>
#include <string>

namespace shell {

// 在 (hwnd, x, y) 显示资源管理器风格右键菜单。
// path 为空时用当前目录 menuDir 生成背景菜单。
// 返回是否执行了操作（用于刷新列表）。
bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen, const std::wstring& customItem,
                     bool& customItemSelected);

// Opens the editable UTF-8 filter list in Notepad, creating it with defaults if needed.
bool OpenContextMenuFilterSettings(HWND owner);

} // namespace shell
