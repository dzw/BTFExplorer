#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace shell {

// 常用资源管理器文件操作，实现依据原版反编译证据（D:\APrj\BinDir\QDirRef）：
//   新建文件夹  0x406DC3/0x422515  CreateDirectoryW + 同名递增
//   快捷方式    0x407F1F           IShellLinkW(00021401-…) + IPersistFile 保存 "%s\%s.lnk"(0x4BAEAC)
//   硬链接      0x45DE2A           mklink /H（0x4C12DC 的 "/H /J" 片段），需 SeCreateLinkPrivilege
//   Junction    0x45DE2A           同一处的 /J；原生 reparse point 写法不需要管理员，优先用它
//   属性        0x491500           ShellExecuteExW fMask=IDLIST, lpVerb="properties"(0x4C0340)
//   拖出        0x493929/0x460EF1  SHDoDragDrop(…, grfEffects=7 即 COPY|MOVE|LINK, …)

// 在 directory 下建“新建文件夹”，重名递增序号；返回新建的完整路径（失败为空）。
std::wstring CreateNewFolder(HWND owner, const std::wstring& directory);

// 建 .lnk 快捷方式。directory 为空时放在目标所在目录（拖放“建快捷方式”时
// 由调用方给目标目录）。返回新建路径（失败为空）。
std::wstring CreateShortcut(HWND owner, const std::wstring& targetPath,
                            const std::wstring& directory = {});

// 为文件建硬链接（目标是目录时失败）；返回新建路径（失败为空）。
// 名字避开 CreateHardLink：UNICODE 下它是 Win32 API CreateHardLinkW 的宏名。
std::wstring CreateFileHardLink(HWND owner, const std::wstring& targetFile);

// 为目录建 junction（目标是文件时失败）；返回新建路径（失败为空）。
std::wstring CreateJunction(HWND owner, const std::wstring& targetDir);

// 弹属性对话框（原版 Alt+Enter / 菜单“属性”：verb 交给 shell 处理）
bool ShowProperties(HWND owner, const std::wstring& path);

// 从 paths 发起拖拽（COPY|MOVE|LINK）。返回最终效果，DROPEFFECT_NONE 表示没拖成。
DWORD DragFilesOut(HWND owner, const std::vector<std::wstring>& paths);

} // namespace shell
