#include "ShellFileOperation.h"
#include "ShellUtil.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>

namespace shell {

bool ExecuteFileOp(HWND hwnd, FileOp op,
                   const std::wstring& srcPath,
                   const std::wstring& destName)
{
    ComPtr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fo))))
        return false;

    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (op != FileOp::Delete) flags |= FOF_NO_CONNECTED_ELEMENTS;
    if (FAILED(fo->SetOperationFlags(flags)))
        return false;

    UniquePIDL pidl = PIDLFromPath(srcPath);
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(srcPath.c_str(), nullptr,
                                           IID_PPV_ARGS(&item))))
        return false;

    HRESULT hr = E_FAIL;
    switch (op) {
    case FileOp::Delete:
        hr = fo->DeleteItem(item.Get(), nullptr);
        break;
    case FileOp::Rename:
        hr = fo->RenameItem(item.Get(), destName.c_str(), nullptr);
        break;
    case FileOp::Copy:
    case FileOp::Move: {
        // destName 在这里表示目标目录
        ComPtr<IShellItem> dest;
        if (FAILED(SHCreateItemFromParsingName(destName.c_str(), nullptr,
                                               IID_PPV_ARGS(&dest))))
            return false;
        hr = (op == FileOp::Copy) ? fo->CopyItem(item.Get(), dest.Get(), nullptr, nullptr)
                                  : fo->MoveItem(item.Get(), dest.Get(), nullptr, nullptr);
        break;
    }
    }
    if (FAILED(hr)) return false;
    hr = fo->PerformOperations();
    return SUCCEEDED(hr);
}

} // namespace shell
