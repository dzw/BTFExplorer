#include "DirectoryTree.h"
#include "../Shell/ShellUtil.h"
#include <shellapi.h>
#include <cwchar>
#include <iterator>

HWND DirectoryTree::Create(HWND parent, int controlId, HFONT font)
{
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), instance, nullptr);
    if (!hwnd_) return nullptr;
    if (font) SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    SHFILEINFOW fileInfo{};
    HIMAGELIST images = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, &fileInfo, sizeof(fileInfo),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    if (images) TreeView_SetImageList(hwnd_, images, TVSIL_NORMAL);

    // 子类化：TreeView 没有中键点击通知，中键在子类过程里捕获
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    defaultProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&DirectoryTree::TreeProcStatic)));
    return hwnd_;
}

LRESULT CALLBACK DirectoryTree::TreeProcStatic(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<DirectoryTree*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->TreeProc(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT DirectoryTree::TreeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MBUTTONDOWN && onMiddleClick_) {
        // 中键点在节点上（图标/文字，不含展开按钮）-> 回调路径
        TVHITTESTINFO hit{};
        hit.pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        if (TreeView_HitTest(h, &hit) && (hit.flags & TVHT_ONITEM) && hit.hItem) {
            TVITEMW item{};
            item.hItem = hit.hItem;
            item.mask = TVIF_PARAM;
            if (TreeView_GetItem(h, &item) && item.lParam) {
                onMiddleClick_(*reinterpret_cast<const std::wstring*>(item.lParam));
                return 0;
            }
        }
    }
    return CallWindowProcW(defaultProc_, h, msg, wp, lp);
}

void DirectoryTree::SetMiddleClickCallback(std::function<void(const std::wstring&)> callback)
{
    onMiddleClick_ = std::move(callback);
}

void DirectoryTree::PopulateDrives()
{
    if (!hwnd_) return;
    wchar_t drives[512]{};
    DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
    if (!length || length >= std::size(drives)) return;

    for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) {
        std::wstring root(drive);
        if (root.size() >= 2 && root[1] == L'\\') root.resize(2);
        root += L'\\';
        int icon = shell::SysIconIndexForEntry(root, true);

        auto* path = new std::wstring(root);
        TVINSERTSTRUCTW insert{};
        insert.hParent = TVI_ROOT;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE |
                           TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        insert.item.pszText = root.data();
        insert.item.lParam = reinterpret_cast<LPARAM>(path);
        insert.item.iImage = insert.item.iSelectedImage = icon >= 0 ? icon : 0;
        insert.item.cChildren = 1;
        if (!TreeView_InsertItem(hwnd_, &insert)) delete path;
    }
}

static bool InsertChildFolders(HWND tree, HTREEITEM parent, const std::wstring& directory)
{
    std::wstring pattern = directory;
    if (pattern.empty() || pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
        FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) return false;

    bool inserted = false;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (data.cFileName[0] == L'.' &&
            (data.cFileName[1] == 0 ||
             (data.cFileName[1] == L'.' && data.cFileName[2] == 0)))
            continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;

        std::wstring name = data.cFileName;
        std::wstring path = directory;
        if (path.empty() || path.back() != L'\\') path += L'\\';
        path += name;
        int icon = shell::SysIconIndexForEntry(path, true);

        auto* itemPath = new std::wstring(path);
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_SORT;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE |
                           TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        insert.item.pszText = name.data();
        insert.item.lParam = reinterpret_cast<LPARAM>(itemPath);
        insert.item.iImage = insert.item.iSelectedImage = icon >= 0 ? icon : 0;
        insert.item.cChildren = 1;
        if (TreeView_InsertItem(tree, &insert))
            inserted = true;
        else
            delete itemPath;
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return inserted;
}

void DirectoryTree::ExpandNode(HTREEITEM item)
{
    HTREEITEM firstChild = TreeView_GetChild(hwnd_, item);
    if (firstChild) {
        TVITEMW child{};
        child.hItem = firstChild;
        child.mask = TVIF_PARAM;
        if (TreeView_GetItem(hwnd_, &child) && child.lParam) return;
    }

    TVITEMW current{};
    current.hItem = item;
    current.mask = TVIF_PARAM;
    if (!TreeView_GetItem(hwnd_, &current) || !current.lParam) return;
    const auto directory = *reinterpret_cast<const std::wstring*>(current.lParam);

    for (HTREEITEM child = TreeView_GetChild(hwnd_, item); child; ) {
        HTREEITEM next = TreeView_GetNextSibling(hwnd_, child);
        TreeView_DeleteItem(hwnd_, child);
        child = next;
    }

    if (!InsertChildFolders(hwnd_, item, directory)) {
        TVITEMW update{};
        update.hItem = item;
        update.mask = TVIF_CHILDREN;
        update.cChildren = 0;
        TreeView_SetItem(hwnd_, &update);
    }
}

bool DirectoryTree::HandleNotification(const NMHDR* notification,
                                       std::wstring& selectedPath)
{
    selectedPath.clear();
    if (!notification || notification->hwndFrom != hwnd_) return false;

    if (notification->code == TVN_DELETEITEMW) {
        const auto* treeNotification =
            reinterpret_cast<const NMTREEVIEWW*>(notification);
        delete reinterpret_cast<std::wstring*>(treeNotification->itemOld.lParam);
        return true;
    }
    if (notification->code == TVN_ITEMEXPANDINGW) {
        const auto* treeNotification =
            reinterpret_cast<const NMTREEVIEWW*>(notification);
        if (treeNotification->action & TVE_EXPAND)
            ExpandNode(treeNotification->itemNew.hItem);
        return true;
    }
    if (notification->code == TVN_SELCHANGEDW) {
        const auto* treeNotification =
            reinterpret_cast<const NMTREEVIEWW*>(notification);
        if (!syncingSelection_ && treeNotification->itemNew.lParam) {
            selectedPath = *reinterpret_cast<const std::wstring*>(
                treeNotification->itemNew.lParam);
        }
        return true;
    }
    return false;
}

void DirectoryTree::SyncToPath(const std::wstring& rawPath, bool showErrors)
{
    std::wstring target(rawPath.c_str());
    if (target.empty() || !hwnd_) return;
    for (wchar_t& ch : target)
        if (ch == L'/') ch = L'\\';
    if (target.rfind(L"\\\\?\\UNC\\", 0) == 0)
        target = L"\\\\" + target.substr(8);
    else if (target.rfind(L"\\\\?\\", 0) == 0 || target.rfind(L"\\??\\", 0) == 0)
        target.erase(0, 4);

    HTREEITEM node = nullptr;
    for (HTREEITEM root = TreeView_GetRoot(hwnd_); root;
         root = TreeView_GetNextSibling(hwnd_, root)) {
        TVITEMW item{};
        item.hItem = root;
        item.mask = TVIF_PARAM;
        if (!TreeView_GetItem(hwnd_, &item) || !item.lParam) continue;
        const auto* rootPath = reinterpret_cast<const std::wstring*>(item.lParam);
        if (rootPath->size() >= 2 && (*rootPath)[1] == L':' &&
            target.size() >= 2 && target[1] == L':' &&
            _wcsnicmp(target.c_str(), rootPath->c_str(), 2) == 0) {
            node = root;
            break;
        }
    }

    if (!node) {
        if (showErrors)
            MessageBoxW(GetParent(hwnd_), L"当前路径不在目录树的本地磁盘范围内。",
                        L"同步目录树", MB_OK | MB_ICONINFORMATION);
        return;
    }

    size_t pos = target.size() >= 3 && target[2] == L'\\' ? 3 : 2;
    while (pos < target.size()) {
        while (pos < target.size() && target[pos] == L'\\') ++pos;
        if (pos >= target.size()) break;
        size_t end = target.find(L'\\', pos);
        if (end == std::wstring::npos) end = target.size();
        std::wstring component = target.substr(pos, end - pos);
        std::wstring expectedPath = target.substr(0, end);

        ExpandNode(node);
        TreeView_Expand(hwnd_, node, TVE_EXPAND);
        HTREEITEM child = nullptr;
        for (HTREEITEM candidate = TreeView_GetChild(hwnd_, node); candidate;
             candidate = TreeView_GetNextSibling(hwnd_, candidate)) {
            wchar_t name[512]{};
            TVITEMW item{};
            item.hItem = candidate;
            item.mask = TVIF_TEXT | TVIF_PARAM;
            item.pszText = name;
            item.cchTextMax = static_cast<int>(std::size(name));
            if (!TreeView_GetItem(hwnd_, &item)) continue;
            if (_wcsicmp(name, component.c_str()) == 0) {
                child = candidate;
                break;
            }
        }

        if (!child && GetFileAttributesW(expectedPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            int icon = shell::SysIconIndexForEntry(expectedPath, true);
            auto* path = new std::wstring(expectedPath);
            TVINSERTSTRUCTW insert{};
            insert.hParent = node;
            insert.hInsertAfter = TVI_SORT;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE |
                               TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
            insert.item.pszText = component.data();
            insert.item.lParam = reinterpret_cast<LPARAM>(path);
            insert.item.iImage = insert.item.iSelectedImage = icon >= 0 ? icon : 0;
            insert.item.cChildren = 1;
            child = TreeView_InsertItem(hwnd_, &insert);
            if (!child) delete path;
        }

        if (!child) {
            if (showErrors) {
                std::wstring message = L"无法在目录树中找到目录：\r\n" + expectedPath;
                MessageBoxW(GetParent(hwnd_), message.c_str(), L"同步目录树",
                            MB_OK | MB_ICONWARNING);
            }
            return;
        }
        node = child;
        pos = end;
    }

    syncingSelection_ = true;
    TreeView_SelectItem(hwnd_, node);
    syncingSelection_ = false;
    TreeView_EnsureVisible(hwnd_, node);
}
