#pragma once
#include <windows.h>
#include <string>

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

} // namespace shell
