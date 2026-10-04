#include "ShellFileOperation.h"
#include "ShellUtil.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include "../Util/AppLog.h"

namespace shell {

static const wchar_t* FileOpName(FileOp op)
{
    switch (op) {
    case FileOp::Delete: return L"delete";
    case FileOp::Copy: return L"copy";
    case FileOp::Move: return L"move";
    case FileOp::Rename: return L"rename";
    }
    return L"unknown";
}

bool ExecuteFileOp(HWND hwnd, FileOp op,
                   const std::wstring& srcPath,
                   const std::wstring& destName)
{
    WriteAppLog((L"FILEOP start: op=" + std::wstring(FileOpName(op)) +
                 L", source=" + srcPath + L", destination=" + destName).c_str());
    ComPtr<IFileOperation> fo;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&fo));
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP CoCreateInstance failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return false;
    }

    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (op != FileOp::Delete) flags |= FOF_NO_CONNECTED_ELEMENTS;
    hr = fo->SetOperationFlags(flags);
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP SetOperationFlags failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return false;
    }

    UniquePIDL pidl = PIDLFromPath(srcPath);
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromParsingName(srcPath.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP source parsing failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr)) +
                     L", source=" + srcPath).c_str());
        return false;
    }

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
        hr = SHCreateItemFromParsingName(destName.c_str(), nullptr, IID_PPV_ARGS(&dest));
        if (FAILED(hr)) {
            WriteAppLog((L"FILEOP destination parsing failed: hr=" +
                         std::to_wstring(static_cast<unsigned long>(hr)) +
                         L", destination=" + destName).c_str());
            return false;
        }
        hr = (op == FileOp::Copy) ? fo->CopyItem(item.Get(), dest.Get(), nullptr, nullptr)
                                  : fo->MoveItem(item.Get(), dest.Get(), nullptr, nullptr);
        break;
    }
    }
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP queue operation failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return false;
    }
    hr = fo->PerformOperations();
    if (FAILED(hr)) {
        WriteAppLog((L"FILEOP PerformOperations failed: hr=" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return false;
    }
    BOOL aborted = FALSE;
    HRESULT abortResult = fo->GetAnyOperationsAborted(&aborted);
    WriteAppLog((L"FILEOP complete: op=" + std::wstring(FileOpName(op)) +
                 L", aborted=" + (SUCCEEDED(abortResult) && aborted ? L"true" : L"false") +
                 L", abortQueryHr=" +
                 std::to_wstring(static_cast<unsigned long>(abortResult))).c_str());
    return true;
}

bool ExecuteFileOpMulti(HWND hwnd, FileOp op,
                        const std::vector<std::wstring>& srcPaths,
                        const std::wstring& destDir,
                        bool recycle)
{
    if (srcPaths.empty()) return false;
    if (op == FileOp::Rename) return false;   // 批量不支持重命名

    ComPtr<IFileOperation> fo;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&fo))))
        return false;

    DWORD flags = FOF_NOCONFIRMMKDIR;
    if (op == FileOp::Delete) {
        if (recycle) flags |= FOF_ALLOWUNDO;   // Shift+Delete 不加 -> 直接删除
    } else {
        flags |= FOF_NO_CONNECTED_ELEMENTS;
    }
    if (FAILED(fo->SetOperationFlags(flags)))
        return false;

    ComPtr<IShellItem> dest;
    if (op == FileOp::Copy || op == FileOp::Move) {
        if (FAILED(SHCreateItemFromParsingName(destDir.c_str(), nullptr,
                                               IID_PPV_ARGS(&dest))))
            return false;
    }

    // IFileOperation 支持先把多个操作排队、最后 PerformOperations 一次执行，
    // 进度/冲突 UI 会把它们当作一批展示
    HRESULT hr = S_OK;
    for (const auto& p : srcPaths) {
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(p.c_str(), nullptr,
                                               IID_PPV_ARGS(&item))))
            continue;
        switch (op) {
        case FileOp::Delete: hr = fo->DeleteItem(item.Get(), nullptr); break;
        case FileOp::Copy:   hr = fo->CopyItem(item.Get(), dest.Get(), nullptr, nullptr); break;
        case FileOp::Move:   hr = fo->MoveItem(item.Get(), dest.Get(), nullptr, nullptr); break;
        default: hr = E_FAIL; break;
        }
        if (FAILED(hr)) return false;
    }
    return SUCCEEDED(fo->PerformOperations());
}

} // namespace shell
