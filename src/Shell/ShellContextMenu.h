#pragma once
#include <windows.h>
#include <string>

namespace shell {

// 在 (hwnd, x, y) 显示资源管理器风格右键菜单。
// path 为空时用当前目录 menuDir 生成背景菜单。
// copyItemLabel 非空时在“Open”组下面插入一项，点击把 copyItemText 写入剪贴板
//（CF_UNICODETEXT，多选用回车换行分隔）。
// 返回是否执行了操作（用于刷新列表）。
bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen, const std::wstring& customItem,
                     bool& customItemSelected,
                     std::wstring* createdFolderPath = nullptr,
                     bool* renameSelected = nullptr,
                     const std::wstring& openInExplorerItem = {},
                     const std::wstring& copyItemLabel = {},
                     const std::wstring& copyItemText = {});

// Opens the editable UTF-8 filter list in Notepad, creating it with defaults if needed.
bool OpenContextMenuFilterSettings(HWND owner);

} // namespace shell
