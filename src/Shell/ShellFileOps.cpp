#include "ShellFileOps.h"
#include "ShellUtil.h"
#include "../Util/AppLog.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winioctl.h>
#include <string>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace shell {

namespace {

void ReportError(HWND owner, const wchar_t* title, const std::wstring& path, DWORD error)
{
    WriteAppLog((std::wstring(L"FILEOP ") + title + L" failed: path=" + path +
                 L", win32Error=" + std::to_wstring(error)).c_str());
    std::wstring message = std::wstring(L"无法完成：\n") + path +
        L"\n错误码：" + std::to_wstring(error);
    MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

std::wstring ParentOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash);
}

std::wstring NameOf(const std::wstring& path)
{
    return path.substr(path.find_last_of(L'\\') + 1);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
{
    std::wstring out = directory;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += name;
    return out;
}

// 目标名已存在时按“名称 (2)”“名称 (3)”递增（原版新建文件夹同款递增）
std::wstring UniquePath(const std::wstring& directory, const std::wstring& baseName,
                        const std::wstring& extension)
{
    for (unsigned int suffix = 1; suffix <= 10000; ++suffix) {
        std::wstring name = baseName;
        if (suffix > 1) name += L" (" + std::to_wstring(suffix) + L")";
        std::wstring candidate = JoinPath(directory, name + extension);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

void NotifyCreated(DWORD event, const std::wstring& path)
{
    SHChangeNotify(event, SHCNF_PATHW, path.c_str(), nullptr);
}

bool IsDirectory(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 绝对 PIDL 的最后一个子项（等价 ILFindLastChild；本 SDK 的 shlwapi 没导出该宏化的
// IDList 辅助函数，这里按 SHITEMID 链自己走）。返回的指针属于 absolute，不单独释放。
const ITEMIDLIST* LastChildOf(const ITEMIDLIST* absolute)
{
    if (!absolute) return nullptr;
    const BYTE* p = reinterpret_cast<const BYTE*>(absolute);
    const BYTE* last = nullptr;
    for (;;) {
        WORD size;
        memcpy(&size, p, sizeof(size));
        if (size == 0) break;              // 结束标记
        last = p;
        p += size;
    }
    return last ? reinterpret_cast<const ITEMIDLIST*>(last) : nullptr;
}

// Junction 用的 reparse 数据（winioctl.h 的 REPARSE_DATA_BUFFER 在本工程工具集里取不到，
// 这里按公开布局摆一遍：8 字节通用头 + 4 个 USHORT 偏移/长度 + 两条以 NUL 结尾的路径）
#pragma pack(push, 2)
struct MountPointData {
    ULONG reparseTag;
    USHORT reparseDataLength;
    USHORT reserved;
    USHORT substituteOffset;
    USHORT substituteLength;
    USHORT printOffset;
    USHORT printLength;
    WCHAR pathBuffer[1];
};
#pragma pack(pop)
static constexpr USHORT kMountPointHeaderTail = static_cast<USHORT>(4 * sizeof(USHORT));
// 名称区之前的固定字节数：通用头 8 + 4 个 USHORT。必须按这个常量算缓冲区大小，
// 不能用 sizeof(MountPointData)——结构体尾部对齐会把大小撑大，
// 而内核要求 InBufferSize 恰好等于 8 + ReparseDataLength，多一个字节就失败。
static constexpr size_t kMountPointNameOffset = 16;

// 硬链接需要 SeCreateLinkPrivilege，标准用户默认没有；原版正是因此走 runas 提权
// （0x45DE2A: ShellExecuteW(verb=L"runas", L"mklink", 参数, SW_SHOW)）。mklink 是
// cmd 内置命令，必须借 cmd.exe 才能执行。
bool CreateHardLinkElevated(HWND owner, const std::wstring& linkPath, const std::wstring& target)
{
    std::wstring params = L"/c mklink /H \"" + linkPath + L"\" \"" + target + L"\"";
    HINSTANCE r = ShellExecuteW(owner, L"runas", L"cmd.exe", params.c_str(),
                                ParentOf(linkPath).c_str(), SW_SHOWNORMAL);
    WriteAppLog((L"FILEOP hardlink elevated: result=" +
                 std::to_wstring(reinterpret_cast<INT_PTR>(r))).c_str());
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace

std::wstring CreateNewFolder(HWND owner, const std::wstring& directory)
{
    if (directory.empty()) return {};
    for (unsigned int suffix = 1; suffix <= 10000; ++suffix) {
        std::wstring name = L"新建文件夹";
        if (suffix > 1) name += L" (" + std::to_wstring(suffix) + L")";
        std::wstring path = JoinPath(directory, name);
        if (CreateDirectoryW(path.c_str(), nullptr)) {
            WriteAppLog((L"NEW_FOLDER created: " + path).c_str());
            NotifyCreated(SHCNE_MKDIR, path);
            return path;
        }
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            ReportError(owner, L"新建文件夹", path, error);
            return {};
        }
    }
    MessageBoxW(owner, L"无法找到可用的新文件夹名称。", L"新建文件夹", MB_OK | MB_ICONERROR);
    return {};
}

std::wstring CreateShortcut(HWND owner, const std::wstring& targetPath,
                             const std::wstring& destination)
{
    if (targetPath.empty() || GetFileAttributesW(targetPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    std::wstring targetDir = ParentOf(targetPath);
    std::wstring directory = destination.empty() ? targetDir : destination;
    std::wstring base = NameOf(targetPath);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) base.erase(dot);   // 主名不含扩展名
    std::wstring linkPath = UniquePath(directory, base + L" - 快捷方式", L".lnk");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的快捷方式名称。", L"创建快捷方式", MB_OK | MB_ICONERROR);
        return {};
    }

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));   // 0x407F1F 用的同一对 CLSID/IID
    if (SUCCEEDED(hr)) hr = link->SetPath(targetPath.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(targetDir.c_str());
    ComPtr<IPersistFile> persist;
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr)) hr = persist->Save(linkPath.c_str(), TRUE);
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP shortcut failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr)) +
                     L", path=" + linkPath).c_str());
        MessageBoxW(owner, L"创建快捷方式失败。", L"创建快捷方式", MB_OK | MB_ICONERROR);
        return {};
    }
    WriteAppLog((L"FILEOP shortcut created: " + linkPath + L" -> " + targetPath).c_str());
    NotifyCreated(SHCNE_CREATE, linkPath);
    return linkPath;
}

std::wstring CreateFileHardLink(HWND owner, const std::wstring& targetFile)
{
    if (targetFile.empty() || IsDirectory(targetFile)) {
        WriteAppLog((L"FILEOP hardlink rejected (not a file): " + targetFile).c_str());
        return {};
    }
    std::wstring directory = ParentOf(targetFile);
    std::wstring linkPath = UniquePath(directory, NameOf(targetFile) + L" 硬链接", L"");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的硬链接名称。", L"创建硬链接", MB_OK | MB_ICONERROR);
        return {};
    }
    if (::CreateHardLinkW(linkPath.c_str(), targetFile.c_str(), nullptr)) {
        WriteAppLog((L"FILEOP hardlink created: " + linkPath + L" -> " + targetFile).c_str());
        NotifyCreated(SHCNE_CREATE, linkPath);
        return linkPath;
    }
    DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
        // 无建链权限：退到原版的提权 mklink /H
        if (CreateHardLinkElevated(owner, linkPath, targetFile) &&
            GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            NotifyCreated(SHCNE_CREATE, linkPath);
            return linkPath;
        }
        return {};
    }
    ReportError(owner, L"创建硬链接", linkPath, error);
    return {};
}

std::wstring CreateJunction(HWND owner, const std::wstring& targetDir)
{
    if (targetDir.empty() || !IsDirectory(targetDir)) {
        WriteAppLog((L"FILEOP junction rejected (not a directory): " + targetDir).c_str());
        return {};
    }
    std::wstring directory = ParentOf(targetDir);
    std::wstring linkPath = UniquePath(directory, NameOf(targetDir) + L" Junction", L"");
    if (linkPath.empty()) {
        MessageBoxW(owner, L"无法找到可用的 Junction 名称。", L"创建 Junction", MB_OK | MB_ICONERROR);
        return {};
    }
    if (!CreateDirectoryW(linkPath.c_str(), nullptr)) {
        ReportError(owner, L"创建 Junction", linkPath, GetLastError());
        return {};
    }

    // IO_REPARSE_TAG_MOUNT_POINT 数据（等价 mklink /J，但不需要管理员权限）。
    // SubstituteName 要用 NT 设备路径形式 "\??\<盘>:\..."。
    std::wstring print = targetDir;
    while (print.size() > 1 && print.back() == L'\\') print.pop_back();
    std::wstring substitute = L"\\??\\" + print;

    const USHORT subBytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    const USHORT printBytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    // 两条路径各带一个结束 NUL，结束符也计入 ReparseDataLength（少算/多算都报
    // ERROR_INVALID_REPARSE_DATA 4392）
    const USHORT tailNulls = static_cast<USHORT>(2 * sizeof(wchar_t));
    const USHORT dataLen =
        static_cast<USHORT>(kMountPointHeaderTail + subBytes + printBytes + tailNulls);
    const size_t totalBytes = kMountPointNameOffset + subBytes + tailNulls + printBytes;
    std::vector<BYTE> raw(totalBytes, 0);
    auto* buffer = reinterpret_cast<MountPointData*>(raw.data());
    buffer->reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer->reparseDataLength = dataLen;
    buffer->substituteLength = subBytes;
    buffer->printOffset = static_cast<USHORT>(subBytes + sizeof(wchar_t));
    buffer->printLength = printBytes;
    memcpy(buffer->pathBuffer, substitute.data(), subBytes);
    memcpy(reinterpret_cast<BYTE*>(buffer->pathBuffer) + buffer->printOffset,
           print.data(), printBytes);

    HANDLE link = CreateFileW(linkPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
    bool ok = false;
    if (link != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        ok = DeviceIoControl(link, FSCTL_SET_REPARSE_POINT, buffer,
                             static_cast<DWORD>(raw.size()), nullptr, 0, &returned, nullptr)
             != FALSE;
        CloseHandle(link);
    }
    if (!ok) {
        DWORD error = GetLastError();
        RemoveDirectoryW(linkPath.c_str());   // 链接目录没成，别留个空壳
        WriteAppLog((L"FILEOP junction failed: path=" + linkPath +
                     L", win32Error=" + std::to_wstring(error)).c_str());
        std::wstring message = L"无法创建 Junction（目标需为本机 NTFS 路径）：\n" + linkPath +
            L"\n错误码：" + std::to_wstring(error);
        MessageBoxW(owner, message.c_str(), L"创建 Junction", MB_OK | MB_ICONERROR);
        return {};
    }
    WriteAppLog((L"FILEOP junction created: " + linkPath + L" -> " + print).c_str());
    NotifyCreated(SHCNE_MKDIR, linkPath);
    return linkPath;
}

bool ShowProperties(HWND owner, const std::wstring& path)
{
    if (path.empty()) return false;
    SHELLEXECUTEINFOW info = { sizeof(info) };
    info.hwnd = owner;
    info.lpVerb = L"properties";
    info.nShow = SW_SHOWNORMAL;
    UniquePIDL pidl = PIDLFromPath(path);
    if (pidl) {
        info.fMask = SEE_MASK_INVOKEIDLIST;
        info.lpIDList = pidl.get();
    } else {
        info.lpFile = path.c_str();   // 虚拟位置取不到 PIDL，按路径交给 shell
    }
    if (!ShellExecuteExW(&info)) {
        WriteAppLog((L"FILEOP properties failed: path=" + path +
                     L", win32Error=" + std::to_wstring(GetLastError())).c_str());
        MessageBoxW(owner, L"无法打开属性对话框。", L"属性", MB_OK | MB_ICONERROR);
        return false;
    }
    WriteAppLog((L"FILEOP properties opened: " + path).c_str());
    return true;
}

DWORD DragFilesOut(HWND owner, const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return DROPEFFECT_NONE;

    // shell 数据对象（CF_HDROP + Shell IDList Array + FileGroupDescriptor），这样
    // 拖到资源管理器/桌面才有完整的“复制/移动/创建快捷方式”语义。数据对象要求
    // “容器 + 子 PIDL”，所以只拖同一目录里的项（文件列表本来就同目录）。
    std::wstring directory = ParentOf(paths.front());
    std::vector<UniquePIDL> owned;
    std::vector<LPCITEMIDLIST> children;
    for (const auto& p : paths) {
        if (ParentOf(p) != directory) continue;
        UniquePIDL absolute = PIDLFromPath(p);
        if (!absolute) continue;
        const ITEMIDLIST* child = LastChildOf(
            reinterpret_cast<const ITEMIDLIST*>(absolute.get()));
        if (!child) continue;
        children.push_back(child);
        owned.push_back(std::move(absolute));   // 子 PIDL 是绝对 PIDL 的后缀，必须留活
    }
    if (children.empty()) return DROPEFFECT_NONE;
    UniquePIDL container = PIDLFromPath(directory);
    if (!container) return DROPEFFECT_NONE;

    ComPtr<IDataObject> data;
    HRESULT hr = SHCreateDataObject(reinterpret_cast<LPCITEMIDLIST>(container.get()),
                                    static_cast<UINT>(children.size()), children.data(),
                                    nullptr, IID_PPV_ARGS(&data));
    if (FAILED(hr) || !data) {
        WriteAppLog((L"FILEOP drag data object failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return DROPEFFECT_NONE;
    }

    DWORD effect = DROPEFFECT_NONE;
    // 原版 grfEffects=7：COPY|MOVE|LINK 全交给目标决定（0x493929）
    hr = SHDoDragDrop(owner, data.Get(), nullptr,
                      DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    WriteAppLog((L"FILEOP drag out: hr=" + std::to_wstring(static_cast<unsigned long>(hr)) +
                 L", effect=" + std::to_wstring(effect)).c_str());
    if (FAILED(hr)) return DROPEFFECT_NONE;
    return effect;
}

} // namespace shell
