#include "ShellUtil.h"
#include "../FileModel/FileEntry.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <mutex>
#include <unordered_map>
#include <cwctype>

namespace shell {

UniquePIDL PIDLFromPath(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF flags = 0;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, &flags)) && pidl)
        return UniquePIDL(pidl);
    return {};
}

std::wstring PathFromPIDL(PCIDLIST_ABSOLUTE pidl) {
    wchar_t buf[MAX_PATH * 2] = {};
    if (pidl && SHGetPathFromIDListW(pidl, buf))
        return buf;
    return {};
}

std::wstring DisplayNameFromPIDL(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return {};
    STRRET sr{};
    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(&desktop))) return {};
    if (SUCCEEDED(desktop->GetDisplayNameOf(pidl, SHGDN_INFOLDER, &sr))) {
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(StrRetToBufW(&sr, pidl, buf, MAX_PATH * 2)))
            return buf;
    }
    return {};
}

HICON GetIconForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    DWORD flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (path.empty() || SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0, &fi, sizeof(fi), flags))
        return fi.hIcon;
    return nullptr;
}

std::wstring TypeNameForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    DWORD flags = SHGFI_TYPENAME;
    if (path.empty() || SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0, &fi, sizeof(fi), flags))
        return fi.szTypeName;
    return {};
}

int SysIconIndexForEntry(const std::wstring& path, bool isFolder) {
    SHFILEINFOW fi{};
    if (!path.empty() && SHGetFileInfoW(path.c_str(), isFolder ? FILE_ATTRIBUTE_DIRECTORY : 0,
                                         &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
        return fi.iIcon;
    return -1;
}

namespace {

// 取文件名部分的扩展名（含点、小写）；无扩展名返回空串。
// 不用 PathFindExtensionW：它对"目录名带点"的路径会取错位置。
std::wstring ExtensionKeyOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring key = path.substr(dot);
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    return key;
}

struct DisplayNameCache {
    std::mutex mtx;
    std::unordered_map<std::wstring, std::wstring> typeByExt;  // 扩展名 -> 类型名
    std::unordered_map<std::wstring, int> iconByExt;           // 扩展名 -> 图标索引
    std::wstring folderTypeName;                               // 文件夹类型名，只查一次
};
DisplayNameCache& Cache() {
    static DisplayNameCache c;
    return c;
}

// 这些类型的图标来自文件内容/目标（每文件不同），按扩展名缓存会张冠李戴
bool IconIsPerFile(const std::wstring& ext) {
    static const wchar_t* kPerFileExts[] = {
        L".exe", L".lnk", L".ico", L".cur", L".ani", L".cpl", L".scr", L".msi",
    };
    for (const wchar_t* e : kPerFileExts)
        if (ext == e) return true;
    return false;
}

} // namespace

std::wstring CachedTypeNameForEntry(const std::wstring& path, bool isFolder) {
    if (path.empty()) return {};
    DisplayNameCache& c = Cache();
    if (isFolder) {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (c.folderTypeName.empty())
            c.folderTypeName = TypeNameForEntry(path, true);
        return c.folderTypeName;
    }
    std::wstring key = ExtensionKeyOf(path);
    {
        std::lock_guard<std::mutex> lk(c.mtx);
        auto it = c.typeByExt.find(key);
        if (it != c.typeByExt.end()) return it->second;
    }
    std::wstring name = TypeNameForEntry(path, false);
    if (name.empty()) return {}; // 查不到的（文件刚被删等）不入缓存，避免空结果被永久记住
    std::lock_guard<std::mutex> lk(c.mtx);
    // 并发下另一线程可能已先插入同 key，emplace 返回已存在的那份即可
    auto it = c.typeByExt.emplace(std::move(key), std::move(name)).first;
    return it->second;
}

int CachedSysIconIndexForEntry(const std::wstring& path, bool isFolder) {
    if (path.empty()) return -1;
    // 文件夹图标可能被 desktop.ini 逐目录定制，不按扩展名缓存
    if (isFolder) return SysIconIndexForEntry(path, true);
    std::wstring key = ExtensionKeyOf(path);
    if (IconIsPerFile(key)) return SysIconIndexForEntry(path, false);
    DisplayNameCache& c = Cache();
    {
        std::lock_guard<std::mutex> lk(c.mtx);
        auto it = c.iconByExt.find(key);
        if (it != c.iconByExt.end()) return it->second;
    }
    int idx = SysIconIndexForEntry(path, false);
    if (idx < 0) return -1; // 同上：失败不缓存
    std::lock_guard<std::mutex> lk(c.mtx);
    c.iconByExt.emplace(std::move(key), idx);
    return idx;
}

} // namespace shell
