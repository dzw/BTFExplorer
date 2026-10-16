#pragma once
#include <windows.h>
#include <string>

namespace shell {

// 右键菜单里本程序自己的文件操作项（原版把这些放在动态追加的子菜单块里，
// 见 QDirRef\decompile\453ED8.c）。哪些项可见由菜单上下文决定：背景菜单只有
// “新建文件夹”，条目菜单再加快捷方式/硬链接(文件)/Junction(目录)。
// 属性不进菜单（shell 原生菜单本来就有“属性”），走 Alt+Enter。
struct MenuCommands {
    bool newFolderSelected = false;
    bool shortcutSelected = false;
    bool hardLinkSelected = false;
    bool junctionSelected = false;
    // 剪贴板里有文件（CF_HDROP）时菜单里会出现“粘贴”，选中后由调用方执行
    // （MainWindow::OnPaste，与 Ctrl+V 同一条代码路径）
    bool pasteSelected = false;
};

// 在 (hwnd, x, y) 显示资源管理器风格右键菜单。
// path 为空时用当前目录 menuDir 生成背景菜单。
// copyItemLabel 非空时在“Open”组下面插入一项，点击把 copyItemText 写入剪贴板
//（CF_UNICODETEXT，多选用回车换行分隔）。
// commands 非空时按 show* 追加本程序的操作项，选中情况回写在同结构里。
// 返回是否执行了操作（用于刷新列表）。
bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen, const std::wstring& customItem,
                     bool& customItemSelected,
                     std::wstring* createdFolderPath = nullptr,
                     bool* renameSelected = nullptr,
                     const std::wstring& openInExplorerItem = {},
                     const std::wstring& copyItemLabel = {},
                     const std::wstring& copyItemText = {},
                     MenuCommands* commands = nullptr);

// Opens the editable UTF-8 filter list in Notepad, creating it with defaults if needed.
bool OpenContextMenuFilterSettings(HWND owner);

} // namespace shell
