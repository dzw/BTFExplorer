#include "DirectoryTree.h"
#include "../Shell/ShellUtil.h"
#include "../Util/AppLog.h"
#include <shlwapi.h>
#include <vector>

// ---------------------------------------------------------------------------
// 事件接收器。INameSpaceTreeControlEvents 是 local 控件接口（shell32 进程内直调），
// 通过 INameSpaceTreeControl::TreeAdvise 挂钩，控件持有本对象的一个引用。
// 回调收到的都是 IShellItem，比旧自建树的通知干净得多。
// ---------------------------------------------------------------------------
class DirectoryTree::EventSink : public INameSpaceTreeControlEvents {
public:
    explicit EventSink(DirectoryTree* owner) : owner_(owner) {}

    // 树析构时先调用：之后即便控件还持引用，回调也不再进入已析构的树
    void Detach() { owner_ = nullptr; }

    // IUnknown（引用计数由控件 TreeAdvise/TreeUnadvise 平衡）
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(INameSpaceTreeControlEvents)) {
            *ppv = static_cast<INameSpaceTreeControlEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG remaining = InterlockedDecrement(&refs_);
        if (!remaining) delete this;
        return remaining;
    }

    // INameSpaceTreeControlEvents（方法与 SDK 头文件声明一一对应；未使用的事件
    // 返回 E_NOTIMPL，控件按“未处理”继续默认行为）
    IFACEMETHODIMP OnItemClick(IShellItem* psi, NSTCEHITTEST /*hitTest*/,
                               NSTCECLICKTYPE clickType) override {
        if (owner_) owner_->OnItemClick(psi, 0, clickType);
        return S_OK;
    }
    IFACEMETHODIMP OnPropertyItemCommit(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemStateChanged(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnSelectionChanged(IShellItemArray* selection) override {
        if (!owner_) return S_OK;
        IShellItem* psi = nullptr;
        if (selection && SUCCEEDED(selection->GetItemAt(0, &psi)) && psi) {
            owner_->OnSelectionChanged(psi);
            psi->Release();
        }
        return S_OK;
    }
    IFACEMETHODIMP OnKeyboardInput(UINT, WPARAM, LPARAM) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeExpand(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnAfterExpand(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeginLabelEdit(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnEndLabelEdit(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnGetToolTip(IShellItem*, LPWSTR, int) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeItemDelete(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemAdded(IShellItem*, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemDeleted(IShellItem*, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeContextMenu(IShellItem*, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID, void**) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeStateImageChange(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnGetDefaultIconIndex(IShellItem*, int*, int*) override { return E_NOTIMPL; }

private:
    ULONG refs_ = 1;
    DirectoryTree* owner_ = nullptr;
};

// ---------------------------------------------------------------------------
// DirectoryTree
// ---------------------------------------------------------------------------

bool DirectoryTree::Create(HWND parent)
{
    // CLSID 用 __uuidof(coclass) 取（头文件带 DECLSPEC_UUID），不用额外链 uuid.lib
    HRESULT hr = CoCreateInstance(__uuidof(NamespaceTreeControl), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ctl_));
    if (FAILED(hr) || !ctl_) {
        WriteAppLog((L"NSTC CoCreateInstance failed hr=0x" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        return false;
    }

    // 资源管理器导航窗格观感：展开钮/连接线/整行选中/常显选中/横向滚动/边框；
    // 关掉标签编辑和拖放（本树只做导航，不做文件操作）
    DWORD style = NSTCS_HASEXPANDOS | NSTCS_HASLINES | NSTCS_FULLROWSELECT |
                  NSTCS_SHOWSELECTIONALWAYS | NSTCS_HORIZONTALSCROLL | NSTCS_AUTOHSCROLL |
                  NSTCS_TABSTOP | NSTCS_NOEDITLABELS | NSTCS_DISABLEDRAGDROP | NSTCS_BORDER;
    RECT rc = { 0, 0, 0, 0 };
    hr = ctl_->Initialize(parent, &rc, style);
    if (FAILED(hr)) {
        WriteAppLog((L"NSTC Initialize failed hr=0x" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        ctl_.Release();
        return false;
    }

    // 挂事件接收器：初始引用 1，TreeAdvise 成功后交还给控件（失败则对象自毁）
    sink_ = new EventSink(this);
    hr = ctl_->TreeAdvise(sink_, &adviseCookie_);
    sink_->Release();
    if (FAILED(hr)) {
        WriteAppLog((L"NSTC TreeAdvise failed hr=0x" +
                     std::to_wstring(static_cast<unsigned long>(hr))).c_str());
        sink_ = nullptr;
        adviseCookie_ = 0;
        ctl_.Release();
        return false;
    }

    // 根 = 命名空间桌面：与资源管理器导航窗格同源（此电脑/驱动器/网络都在其下）
    ComPtr<IShellItem> desktop;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_Desktop, 0, nullptr,
                                         IID_PPV_ARGS(&desktop))) || !desktop) {
        ctl_.Release();
        return false;
    }
    ctl_->AppendRoot(desktop.Get(), SHCONTF_FOLDERS,
                     NSTCRS_VISIBLE | NSTCRS_EXPANDED, nullptr);
    ctl_->SetTheme(L"Explorer");

    // 控件窗口以窗口类 "NamespaceTreeControl" 挂为 parent 子窗口；尺寸交给 Layout
    hwnd_ = FindWindowExW(parent, nullptr, L"NamespaceTreeControl", nullptr);
    if (!hwnd_) {
        WriteAppLog(L"NSTC window not found after Initialize");
        ctl_.Release();
        return false;
    }
    return true;
}

DirectoryTree::~DirectoryTree()
{
    if (sink_) sink_->Detach();   // 先断回调再解绑，防解绑过程中的重入
    if (ctl_ && adviseCookie_) ctl_->TreeUnadvise(adviseCookie_);
    sink_ = nullptr;
}

std::wstring DirectoryTree::PathOfItem(IShellItem* psi)
{
    if (!psi) return {};
    PWSTR psz = nullptr;
    if (FAILED(psi->GetDisplayName(SIGDN_FILESYSPATH, &psz)) || !psz) return {};
    std::wstring path = psz;
    CoTaskMemFree(psz);
    return path;
}

// UniquePIDL 持有的是 void*，调 shell API 前转回 PIDL
static PCIDLIST_ABSOLUTE PidlOf(const UniquePIDL& p)
{
    return static_cast<PCIDLIST_ABSOLUTE>(p.get());
}

void DirectoryTree::OnSelectionChanged(IShellItem* psi)
{
    if (!psi || syncing_) return;      // SyncToPath 自己的选中不当作用户导航
    std::wstring path = PathOfItem(psi);
    if (path.empty()) return;          // 虚拟位置（此电脑/回收站等）不导航
    if (onSelection_) onSelection_(path);
}

void DirectoryTree::OnItemClick(IShellItem* psi, NSTCEHITTEST, NSTCECLICKTYPE clickType)
{
    if (!psi || !onMiddleClick_) return;
    if (!ISMBUTTON(clickType) || ISDBLCLICK(clickType)) return;   // 只处理中键单击
    std::wstring path = PathOfItem(psi);
    if (path.empty()) return;
    onMiddleClick_(path);
}

void DirectoryTree::SyncToPath(const std::wstring& rawPath, bool showErrors)
{
    if (!hwnd_ || !ctl_ || rawPath.empty()) return;
    (void)showErrors;   // 保留参数兼容旧调用点；失败时静默保持树的现状

    std::wstring target(rawPath.c_str());
    for (wchar_t& ch : target)
        if (ch == L'/') ch = L'\\';
    if (target.rfind(L"\\\\?\\UNC\\", 0) == 0)
        target = L"\\\\" + target.substr(8);
    else if (target.rfind(L"\\\\?\\", 0) == 0 || target.rfind(L"\\??\\", 0) == 0)
        target.erase(0, 4);

    UniquePIDL pidl = shell::PIDLFromPath(target.c_str());
    if (!pidl) return;

    // 逐级展开祖先（最外层先展开）。默认非异步模式下展开是同步的，
    // 展开后子项才在树里存在，最后才能选中目标并滚到可见。
    syncing_ = true;

    auto expandPrefix = [&](const std::wstring& prefix) {
        UniquePIDL anc = shell::PIDLFromPath(prefix.c_str());
        if (!anc) return;
        ComPtr<IShellItem> ancItem;
        if (SUCCEEDED(SHCreateItemFromIDList(PidlOf(anc), IID_PPV_ARGS(&ancItem))) && ancItem)
            ctl_->SetItemState(ancItem.Get(), NSTCIS_EXPANDED, NSTCIS_EXPANDED);
    };

    // 根前缀："F:\" 或 UNC "\\server\share"；其下逐段补齐展开，目标本身不展开
    // （与旧自建树行为一致）。段落切分沿用旧树的逻辑。
    size_t pos = std::wstring::npos;
    if (target.size() >= 2 && target[1] == L':') {
        expandPrefix(target.substr(0, 2) + L'\\');
        pos = 3;
    } else if (target.size() >= 3 && target[0] == L'\\' && target[1] == L'\\') {
        size_t share = target.find(L'\\', 2);
        size_t rootEnd = (share == std::wstring::npos)
                             ? std::wstring::npos : target.find(L'\\', share + 1);
        if (rootEnd == std::wstring::npos) rootEnd = target.size();
        expandPrefix(target.substr(0, rootEnd));
        pos = rootEnd;
    }
    if (pos != std::wstring::npos) {
        while (pos < target.size()) {
            while (pos < target.size() && target[pos] == L'\\') ++pos;
            if (pos >= target.size()) break;
            size_t end = target.find(L'\\', pos);
            if (end == std::wstring::npos) end = target.size();
            if (end < target.size())
                expandPrefix(target.substr(0, end));
            pos = end;
        }
    }

    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromIDList(PidlOf(pidl), IID_PPV_ARGS(&item))) && item) {
        ctl_->SetItemState(item.Get(), NSTCIS_SELECTED, NSTCIS_SELECTED);
        ctl_->EnsureItemVisible(item.Get());
    }
    syncing_ = false;
}
