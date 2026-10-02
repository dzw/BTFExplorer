#include "ShellUtil.h"
#include "../FileModel/FileEntry.h"
#include <shellapi.h>
#include <shlwapi.h>

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

} // namespace shell
