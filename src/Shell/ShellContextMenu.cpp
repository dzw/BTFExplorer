#include "ShellContextMenu.h"
#include "ShellUtil.h"
#include "../Util/AppLog.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cerrno>
#include <cstdio>
#include <cwctype>
#include <utility>
#include <vector>

namespace shell {

static constexpr UINT CMD_FIRST = 1;
static constexpr UINT CMD_LAST  = 0x7FFF;
static constexpr UINT CMD_CUSTOM = CMD_LAST + 1;
static constexpr UINT CMD_RENAME = CMD_LAST + 2;
static constexpr UINT CMD_EXPLORER = CMD_LAST + 3; // 在 Explorer 中打开

static const std::vector<std::wstring>& DefaultHiddenNames()
{
    static const std::vector<std::wstring> names = {
        L"tortoisesvn", L"tortoise",
        L"yunshell", L"yunsh",
        L"百度网盘", L"传送到百度", L"发送到百度", L"百度云", L"baidu"
    };
    return names;
}

static std::wstring FilterSettingsPath()
{
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring path(modulePath, length);
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash + 1);
    path += L"context-menu-filters.txt";
    return path;
}

static std::wstring Trim(std::wstring value)
{
    size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool LoadHiddenNames(HWND owner, std::vector<std::wstring>& names)
{
    std::wstring path = FilterSettingsPath();
    if (path.empty()) {
        MessageBoxW(owner, L"无法确定右键菜单过滤配置文件的位置。", L"右键菜单过滤",
                    MB_OK | MB_ICONERROR);
        names = DefaultHiddenNames();
        return false;
    }

    FILE* file = nullptr;
    errno_t error = _wfopen_s(&file, path.c_str(), L"rb");
    if (error != 0 || !file) {
        if (error == ENOENT) {
            names = DefaultHiddenNames();
            return true;
        }
        MessageBoxW(owner, L"无法读取右键菜单过滤配置文件，将暂时使用默认过滤词。",
                    L"右键菜单过滤", MB_OK | MB_ICONWARNING);
        names = DefaultHiddenNames();
        return false;
    }

    std::string data;
    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, bytes);
    bool readFailed = ferror(file) != 0;
    fclose(file);
    if (readFailed) {
        MessageBoxW(owner, L"读取右键菜单过滤配置文件时发生错误，将暂时使用默认过滤词。",
                    L"右键菜单过滤", MB_OK | MB_ICONWARNING);
        names = DefaultHiddenNames();
        return false;
    }

    names.clear();
    size_t pos = (data.size() >= 3 &&
                  static_cast<unsigned char>(data[0]) == 0xEF &&
                  static_cast<unsigned char>(data[1]) == 0xBB &&
                  static_cast<unsigned char>(data[2]) == 0xBF) ? 3 : 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos)
            end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        int charCount = MultiByteToWideChar(CP_UTF8, 0, line.data(),
                                            static_cast<int>(line.size()), nullptr, 0);
        if (charCount <= 0)
            continue;
        std::wstring name(static_cast<size_t>(charCount), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                            name.data(), charCount);
        name = Trim(std::move(name));
        if (!name.empty() && name.front() != L'#')
            names.push_back(std::move(name));
    }
    return true;
}

static bool IsHiddenProviderItem(const std::wstring& text,
                                 const std::vector<std::wstring>& hiddenNames)
{
    std::wstring normalized = text;
    for (wchar_t& ch : normalized)
        ch = static_cast<wchar_t>(std::towlower(ch));
    for (const std::wstring& name : hiddenNames) {
        std::wstring normalizedName = name;
        for (wchar_t& ch : normalizedName)
            ch = static_cast<wchar_t>(std::towlower(ch));
        if (!normalizedName.empty() && normalized.find(normalizedName) != std::wstring::npos)
            return true;
    }
    return false;
}

static bool FilterProviderItems(HMENU menu, const std::vector<std::wstring>& hiddenNames)
{
    bool removed = false;
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) {
        wchar_t text[512]{};
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING | MIIM_SUBMENU;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text));
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item))
            continue;

        if (IsHiddenProviderItem(text, hiddenNames)) {
            DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
            removed = true;
        } else if (item.hSubMenu && FilterProviderItems(item.hSubMenu, hiddenNames)) {
            if (GetMenuItemCount(item.hSubMenu) == 0)
                DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
            removed = true;
        }
    }
    return removed;
}

static bool IsOpenAction(const std::wstring& text)
{
    // 归一化：小写化，并去掉加速键符号 '&'（有的项写作 “Open Git &GUI here”，
    // 带 & 会导致 “open git gui here” 子串匹配失败）。
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'&') continue;
        normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }

    if (normalized.find(L"everything") != std::wstring::npos)
        return false;

    // 下列项一律留在顶层，不并入 Open 分组（默认会被通用规则归组，这里显式排除）：
    //   Notepad++ / Sublime / VSCode —— 文本、代码编辑器，通常带 “Open with …” 前缀
    //   Terminal                     —— “Open in Terminal”，命中通用规则 “open in ”
    //   “通过 Code打开”              —— VSCode 的中文项，命中规则 (code + 打开)
    if (normalized.find(L"notepad++") != std::wstring::npos ||
        normalized.find(L"notepad+") != std::wstring::npos ||
        normalized.find(L"sublime") != std::wstring::npos ||
        normalized.find(L"terminal") != std::wstring::npos ||
        normalized.find(L"vscode") != std::wstring::npos ||
        normalized.find(L"vs code") != std::wstring::npos ||
        normalized.find(L"visual studio code") != std::wstring::npos ||
        (normalized.find(L"code") != std::wstring::npos &&
         normalized.find(L"打开") != std::wstring::npos))
        return false;

    // 反汇编器 IDA 并入 Open 分组（默认它不带 “open” 前缀，只会留在顶层）。
    if (normalized.find(L"ida pro") != std::wstring::npos ||
        normalized.find(L"ida32") != std::wstring::npos ||
        normalized.find(L"ida64") != std::wstring::npos)
        return true;

    // VLC / ChatGPT / Windows Media Player 的相关项也并入 Open 分组（用户要求）：
    //   “Add to VLC media player's Playlist”“Play with VLC media player”
    //   “Open project in ChatGPT”
    //   “添加到 Windows Media Player 列表”“使用 Windows Media Player 播放”
    if (normalized.find(L"vlc media player") != std::wstring::npos ||
        normalized.find(L"chatgpt") != std::wstring::npos ||
        normalized.find(L"windows media player") != std::wstring::npos)
        return true;

    return normalized.find(L"open in ") != std::wstring::npos ||
           normalized.find(L"open with ") != std::wstring::npos ||
           normalized.find(L"open git gui here") != std::wstring::npos ||
           normalized.find(L"open git bash here") != std::wstring::npos ||
           (normalized.find(L"visual studio") != std::wstring::npos &&
            normalized.find(L"打开") != std::wstring::npos) ||
           (normalized.find(L"code") != std::wstring::npos &&
            normalized.find(L"打开") != std::wstring::npos) ||
           (normalized.find(L"workbuddy") != std::wstring::npos &&
            normalized.find(L"打开") != std::wstring::npos);
}

static bool GroupOpenActions(HMENU menu)
{
    struct Entry {
        MENUITEMINFOW info{};
        std::wstring text;
        UINT position = 0;
    };
    std::vector<Entry> moved;
    HMENU submenu = CreatePopupMenu();
    if (!submenu) return false;
    auto insertEntry = [](HMENU target, UINT position, const Entry& entry) {
        MENUITEMINFOW info = entry.info;
        info.dwTypeData = const_cast<LPWSTR>(entry.text.c_str());
        info.cch = static_cast<UINT>(entry.text.size());
        return InsertMenuItemW(target, position, TRUE, &info) != FALSE;
    };
    auto rollback = [&]() {
        for (const Entry& entry : moved) {
            RemoveMenu(submenu, 0, MF_BYPOSITION);
            insertEntry(menu, entry.position, entry);
        }
    };

    for (int i = 0; i < GetMenuItemCount(menu);) {
        std::vector<wchar_t> text(1024, L'\0');
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_SUBMENU |
                     MIIM_DATA | MIIM_BITMAP | MIIM_STRING;
        item.dwTypeData = text.data();
        item.cch = static_cast<UINT>(text.size());
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) {
            ++i;
            continue;
        }
        std::wstring label(item.dwTypeData, item.cch);
        if (!IsOpenAction(label)) {
            ++i;
            continue;
        }

        Entry entry;
        entry.info = item;
        entry.text = std::move(label);
        entry.info.dwTypeData = entry.text.data();
        entry.info.cch = static_cast<UINT>(entry.text.size());
        entry.position = static_cast<UINT>(i);
        if (!RemoveMenu(menu, static_cast<UINT>(i), MF_BYPOSITION)) {
            DestroyMenu(submenu);
            return false;
        }
        if (!insertEntry(submenu, static_cast<UINT>(GetMenuItemCount(submenu)), entry)) {
            insertEntry(menu, static_cast<UINT>(i), entry);
            rollback();
            DestroyMenu(submenu);
            return false;
        }
        moved.push_back(std::move(entry));
    }

    if (moved.empty()) {
        DestroyMenu(submenu);
        return false;
    }

    MENUITEMINFOW openItem{};
    openItem.cbSize = sizeof(openItem);
    openItem.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
    openItem.fType = MFT_STRING;
    openItem.dwTypeData = const_cast<LPWSTR>(L"Open");
    openItem.hSubMenu = submenu;
    if (!InsertMenuItemW(menu, moved.front().position, TRUE, &openItem)) {
        rollback();
        DestroyMenu(submenu);
        return false;
    }
    return true;
}

static std::wstring MenuItemText(HMENU menu, UINT position)
{
    std::vector<wchar_t> text(1024, L'\0');
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_STRING;
    item.dwTypeData = text.data();
    item.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoW(menu, position, TRUE, &item))
        return {};
    return std::wstring(item.dwTypeData, item.cch);
}

static bool IsRenameLabel(const std::wstring& text)
{
    std::wstring normalized = text;
    for (wchar_t& ch : normalized) {
        if (ch == L'&') ch = L' ';
        else ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return normalized.find(L"重命名") != std::wstring::npos ||
           normalized.find(L"rename") != std::wstring::npos;
}

static void RemoveRenameItems(HMENU menu)
{
    for (int i = GetMenuItemCount(menu) - 1; i >= 0; --i) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item))
            continue;

        if (IsRenameLabel(MenuItemText(menu, static_cast<UINT>(i)))) {
            DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
        } else if (item.hSubMenu) {
            RemoveRenameItems(item.hSubMenu);
            if (GetMenuItemCount(item.hSubMenu) == 0)
                DeleteMenu(menu, static_cast<UINT>(i), MF_BYPOSITION);
        }
    }
}

static bool IsNewFolderCommand(HMENU menu, UINT command, bool inNewMenu = false)
{
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item))
            continue;

        std::wstring text = MenuItemText(menu, static_cast<UINT>(i));
        std::wstring normalized = text;
        for (wchar_t& ch : normalized) {
            if (ch == L'&') ch = L' ';
            else ch = static_cast<wchar_t>(std::towlower(ch));
        }
        bool isNewMenu = inNewMenu ||
            normalized.find(L"新建") != std::wstring::npos ||
            normalized.find(L"new") != std::wstring::npos;
        if (item.hSubMenu) {
            if (IsNewFolderCommand(item.hSubMenu, command, isNewMenu))
                return true;
        } else if (item.wID == command && isNewMenu &&
                   (normalized.find(L"文件夹") != std::wstring::npos ||
                    normalized.find(L"folder") != std::wstring::npos)) {
            return true;
        }
    }
    return false;
}

static std::wstring CreateFolderForRename(HWND owner, const std::wstring& directory)
{
    if (directory.empty()) return {};
    for (unsigned int suffix = 1; suffix <= 10000; ++suffix) {
        std::wstring name = L"新建文件夹";
        if (suffix > 1)
            name += L" (" + std::to_wstring(suffix) + L")";
        std::wstring path = directory;
        if (path.back() != L'\\' && path.back() != L'/')
            path.push_back(L'\\');
        path += name;
        if (CreateDirectoryW(path.c_str(), nullptr)) {
            WriteAppLog((L"NEW_FOLDER created: " + path).c_str());
            return path;
        }
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            WriteAppLog((L"NEW_FOLDER create failed: path=" + path +
                         L", win32Error=" + std::to_wstring(error)).c_str());
            std::wstring message = L"无法创建文件夹：\n" + path +
                L"\n错误码：" + std::to_wstring(error);
            MessageBoxW(owner, message.c_str(), L"新建文件夹", MB_OK | MB_ICONERROR);
            return {};
        }
    }
    MessageBoxW(owner, L"无法找到可用的新文件夹名称。",
                L"新建文件夹", MB_OK | MB_ICONERROR);
    WriteAppLog((L"NEW_FOLDER failed: no available name in " + directory).c_str());
    return {};
}

struct ContextMenuHandler {
    IContextMenu2* menu2 = nullptr;
    IContextMenu3* menu3 = nullptr;
};

static LRESULT CALLBACK ContextMenuSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                                 LPARAM lp, UINT_PTR,
                                                 DWORD_PTR refData)
{
    auto* handler = reinterpret_cast<ContextMenuHandler*>(refData);
    if (handler &&
        (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM ||
         msg == WM_MEASUREITEM || msg == WM_MENUCHAR)) {
        LRESULT result = 0;
        if (handler->menu3 && msg == WM_MENUCHAR &&
            SUCCEEDED(handler->menu3->HandleMenuMsg2(msg, wp, lp, &result)))
            return result;
        IContextMenu2* menu2 = handler->menu3
            ? static_cast<IContextMenu2*>(handler->menu3) : handler->menu2;
        if (menu2 && SUCCEEDED(menu2->HandleMenuMsg(msg, wp, lp)))
            return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool ShowContextMenu(HWND hwnd, const std::wstring& path, const std::wstring& menuDir,
                     POINT ptScreen, const std::wstring& customItem,
                     bool& customItemSelected, std::wstring* createdFolderPath,
                     bool* renameSelected, const std::wstring& openInExplorerItem)
{
    customItemSelected = false;
    if (createdFolderPath) createdFolderPath->clear();
    if (renameSelected) *renameSelected = false;
    WriteAppLog((L"CONTEXT_MENU opening: path=" +
                 (path.empty() ? std::wstring(L"(background)") : path) +
                 L", directory=" + menuDir).c_str());
    UniquePIDL pidl;
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;

    if (!path.empty()) {
        pidl = PIDLFromPath(path);
        if (!pidl) return false;
        IShellFolder* pParent = nullptr;
        if (FAILED(SHBindToParent(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.get()), IID_IShellFolder,
                                  reinterpret_cast<void**>(&pParent), &child)))
            return false;
        parent.Attach(pParent);
    } else {
        std::wstring dir = menuDir.empty() ? L"C:\\" : menuDir;
        pidl = PIDLFromPath(dir);
        if (!pidl) return false;
        IShellFolder* pFolder = nullptr;
        if (FAILED(SHBindToObject(nullptr, reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.get()), nullptr, IID_IShellFolder,
                                  reinterpret_cast<void**>(&pFolder))))
            return false;
        parent.Attach(pFolder);
    }

    ComPtr<IContextMenu> cm;
    HRESULT hr;
    if (child) {
        PCUITEMID_CHILD_ARRAY one = &child;
        hr = parent->GetUIObjectOf(hwnd, 1, one, IID_IContextMenu, nullptr,
                                   reinterpret_cast<void**>(cm.operator&()));
    } else {
        hr = parent->CreateViewObject(hwnd, IID_IContextMenu,
                                      reinterpret_cast<void**>(cm.operator&()));
    }
    if (FAILED(hr) || !cm) return false;

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    bool invoked = false;
    if (SUCCEEDED(cm->QueryContextMenu(menu, 0, CMD_FIRST, CMD_LAST, CMF_NORMAL))) {
        std::vector<std::wstring> hiddenNames;
        LoadHiddenNames(hwnd, hiddenNames);
        FilterProviderItems(menu, hiddenNames);
        GroupOpenActions(menu);
        if (!path.empty() && renameSelected) {
            RemoveRenameItems(menu);
            BOOL appended = AppendMenuW(menu, MF_STRING, CMD_RENAME, L"重命名(&M)");
            WriteAppLog(appended
                ? L"CONTEXT_MENU custom rename item appended"
                : (L"CONTEXT_MENU failed to append rename item: error=" +
                   std::to_wstring(GetLastError())).c_str());
        }
        if (!customItem.empty()) {
            if (GetMenuItemCount(menu) > 0)
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CMD_CUSTOM, customItem.c_str());
        }
        if (!openInExplorerItem.empty()) {
            if (GetMenuItemCount(menu) > 0)
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CMD_EXPLORER, openInExplorerItem.c_str());
        }
        ComPtr<IContextMenu3> cm3;
        ComPtr<IContextMenu2> cm2;
        bool hasMenu3 = SUCCEEDED(cm->QueryInterface(
            IID_IContextMenu3, reinterpret_cast<void**>(cm3.operator&())));
        if (!hasMenu3)
            cm->QueryInterface(IID_IContextMenu2,
                               reinterpret_cast<void**>(cm2.operator&()));
        ContextMenuHandler handler{ cm2.Get(), cm3.Get() };
        UINT_PTR subclassId = reinterpret_cast<UINT_PTR>(&ContextMenuSubclassProc);
        bool subclassed = (!cm2 && !cm3) ||
            SetWindowSubclass(hwnd, ContextMenuSubclassProc, subclassId,
                              reinterpret_cast<DWORD_PTR>(&handler)) != FALSE;
        if (!subclassed) {
            MessageBoxW(hwnd, L"无法初始化系统右键菜单，无法显示动态菜单项。",
                        L"右键菜单", MB_OK | MB_ICONERROR);
            DestroyMenu(menu);
            return false;
        }
        UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    ptScreen.x, ptScreen.y, hwnd, nullptr);
        WriteAppLog((L"CONTEXT_MENU returned command=" +
                     std::to_wstring(cmd)).c_str());
        if (subclassed && (cm2 || cm3))
            RemoveWindowSubclass(hwnd, ContextMenuSubclassProc, subclassId);
        if (cmd == CMD_CUSTOM) {
            customItemSelected = true;
        } else if (cmd == CMD_EXPLORER) {
            // 在真实资源管理器中打开：文件 -> /select 定位并选中；文件夹/背景 -> 直接打开目录
            std::wstring target = path.empty()
                ? (menuDir.empty() ? L"C:\\" : menuDir)
                : path;
            std::wstring params;
            DWORD attr = GetFileAttributesW(target.c_str());
            bool isDir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
            if (isDir)
                params = L"\"" + target + L"\"";
            else
                params = L"/select,\"" + target + L"\"";
            HINSTANCE r = ShellExecuteW(hwnd, L"open", L"explorer.exe",
                                        params.c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(r) <= 32) {
                MessageBoxW(hwnd, L"无法打开资源管理器。", L"在 Explorer 中打开",
                            MB_OK | MB_ICONERROR);
            } else {
                WriteAppLog((L"CONTEXT_MENU open in explorer: " + params).c_str());
            }
            invoked = true;
        } else if (cmd == CMD_RENAME) {
            if (renameSelected) *renameSelected = true;
            WriteAppLog((L"CONTEXT_MENU rename command selected: " + path).c_str());
        } else if (cmd >= CMD_FIRST && cmd <= CMD_LAST) {
            // “新建”等文件夹背景菜单处理器靠 lpDirectoryW 知道在哪里创建文件，
            // 不设置会静默失败（粘贴/删除有数据对象提供目标，唯独新建依赖它）
            // 背景菜单（path 为空）用当前目录；选中条目则用其所在目录
            std::wstring invokeDir = path.empty() ? menuDir : path;
            if (!path.empty() && !invokeDir.empty()) {
                std::vector<wchar_t> parentPath(invokeDir.begin(), invokeDir.end());
                parentPath.push_back(L'\0');
                if (PathRemoveFileSpecW(parentPath.data()))
                    invokeDir.assign(parentPath.data());
            }
            bool newFolderCommand = IsNewFolderCommand(menu, cmd);
            WriteAppLog((L"CONTEXT_MENU shell command selected: id=" +
                         std::to_wstring(cmd) + L", newFolder=" +
                         (newFolderCommand ? L"true" : L"false") +
                         L", directory=" + invokeDir).c_str());
            if (newFolderCommand && createdFolderPath) {
                *createdFolderPath = CreateFolderForRename(hwnd, invokeDir);
                invoked = !createdFolderPath->empty();
                WriteAppLog((L"NEW_FOLDER request result: path=" +
                             (createdFolderPath->empty() ? std::wstring(L"(none)") :
                                                           *createdFolderPath)).c_str());
                DestroyMenu(menu);
                return invoked;
            }
            char dirA[MAX_PATH * 2] = {};
            if (!invokeDir.empty())
                WideCharToMultiByte(CP_ACP, 0, invokeDir.c_str(), -1,
                                    dirA, sizeof(dirA), nullptr, nullptr);
            CMINVOKECOMMANDINFOEX info = { sizeof(info) };
            info.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            info.hwnd = hwnd;
            info.lpVerb  = reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(cmd - CMD_FIRST));
            info.lpVerbW = reinterpret_cast<LPCWSTR>(MAKEINTRESOURCEW(cmd - CMD_FIRST));
            info.lpDirectory  = dirA[0] ? dirA : nullptr;
            info.lpDirectoryW = invokeDir.empty() ? nullptr : invokeDir.c_str();
            info.nShow = SW_SHOWNORMAL;
            info.ptInvoke = ptScreen;
            invoked = SUCCEEDED(cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&info)));
            if (!invoked)
                MessageBoxW(hwnd, L"执行所选系统右键菜单命令失败。",
                            L"右键菜单", MB_OK | MB_ICONERROR);
        }
    }
    DestroyMenu(menu);
    return invoked;
}

bool OpenContextMenuFilterSettings(HWND owner)
{
    std::wstring path = FilterSettingsPath();
    if (path.empty()) {
        MessageBoxW(owner, L"无法确定右键菜单过滤配置文件的位置。", L"右键菜单过滤",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    FILE* file = nullptr;
    errno_t error = _wfopen_s(&file, path.c_str(), L"rb");
    if (error == ENOENT) {
        error = _wfopen_s(&file, path.c_str(), L"wb");
        if (error != 0 || !file) {
            MessageBoxW(owner, L"无法创建右键菜单过滤配置文件。", L"右键菜单过滤",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        static const char defaults[] =
            "\xEF\xBB\xBF# 每行一个关键词；菜单文字包含关键词时，该菜单项会被隐藏。\r\n"
            "# 删除关键词即可允许对应菜单项；清空文件可关闭全部过滤。\r\n"
            "tortoisesvn\r\n"
            "tortoise\r\n"
            "yunshell\r\n"
            "yunsh\r\n"
            "百度网盘\r\n"
            "传送到百度\r\n"
            "发送到百度\r\n"
            "百度云\r\n"
            "baidu\r\n";
        size_t length = sizeof(defaults) - 1;
        bool writeFailed = fwrite(defaults, 1, length, file) != length;
        if (fclose(file) != 0)
            writeFailed = true;
        if (writeFailed) {
            MessageBoxW(owner, L"写入右键菜单过滤配置文件失败。", L"右键菜单过滤",
                        MB_OK | MB_ICONERROR);
            return false;
        }
    } else if (error != 0 || !file) {
        MessageBoxW(owner, L"无法打开右键菜单过滤配置文件。", L"右键菜单过滤",
                    MB_OK | MB_ICONERROR);
        return false;
    } else {
        fclose(file);
    }

    std::wstring parameters = L"\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(owner, L"open", L"notepad.exe", parameters.c_str(),
                                     nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(owner, L"无法启动记事本打开右键菜单过滤配置文件。", L"右键菜单过滤",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

} // namespace shell