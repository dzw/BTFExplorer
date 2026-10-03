#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace shell {

enum class FileOp {
    Delete,   // 到回收站
    Copy,
    Move,
    Rename,
};

// 通过 IFileOperation 执行（回收站删除/复制/移动/重命名）。
// 返回是否成功。destName 仅 Rename/Copy/Move 用（目标文件名或目标目录）。
bool ExecuteFileOp(HWND hwnd, FileOp op,
                   const std::wstring& srcPath,
                   const std::wstring& destName);

// 批量版本：Copy/Move 的 destDir 是目标目录；Delete 时 recycle=false 表示
// 不进回收站（Shift+Delete 直接删除）。Rename 不支持批量（返回 false）。
bool ExecuteFileOpMulti(HWND hwnd, FileOp op,
                        const std::vector<std::wstring>& srcPaths,
                        const std::wstring& destDir,
                        bool recycle = true);

} // namespace shell
