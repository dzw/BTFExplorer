#include "DirectoryTree.h"
#include "../Shell/ShellUtil.h"
#include "../Util/AppLog.h"
#include <shlwapi.h>
#include <uxtheme.h>          // SetWindowTheme
#include <vector>

#pragma comment(lib, "UxTheme.lib")

// ---------------------------------------------------------------------------
// 事件接收器。INameSpaceTreeControlEvents 是 local 控件接口（shell32 进程内直调），
// 通过 INameSpaceTreeControl::TreeAdvise 挂钩，控件持有本对象的一个引用。
// 回调收到的都是 IShellItem，比旧自建树的通知干净得多。
//
// 关键：名称里带 "Before" / "Changing" 的是**否决点**（OnBeforeExpand、
// OnBeforeItemDelete、OnBeforeContextMenu、OnItemStateChanging），返回失败码
// （含 E_NOTIMPL）会被控件当成“取消这次操作”，必须返回 S_OK；
// 只有纯通知型事件（OnAfterExpand / OnItemAdded …）才返回 E_NOTIMPL。
// 之前展开按钮点不动就是因为 OnItemStateChanging 回了 E_NOTIMPL——展开本质是
// 项的 NSTCIS_EXPANDED 状态变更，被自己否决掉了。
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

    // INameSpaceTreeControlEvents（方法与 SDK 头文件声明一一对应）
    IFACEMETHODIMP OnItemClick(IShellItem* psi, NSTCEHITTEST /*hitTest*/,
                               NSTCECLICKTYPE clickType) override {
        if (owner_) owner_->OnItemClick(psi, 0, clickType);
        return S_OK;
    }
    IFACEMETHODIMP OnPropertyItemCommit(IShellItem*) override { return E_NOTIMPL; }
    // 否决点：必须 S_OK，否则项的展开/选中状态变更会被取消
    IFACEMETHODIMP OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) override { return S_OK; }
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
    // 否决点：必须 S_OK，否则不展开
    IFACEMETHODIMP OnBeforeExpand(IShellItem*) override { return S_OK; }
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
    // 关掉标签编辑和拖放（本树只做导航，不做文件操作）。
    // NSTCS_HASEXPANDOS 始终保留（展开图标始终存在），它渲染成三角还是 +/- 方框
    // 由内层 TreeView 的主题决定，见 SetClassicExpandIcons。
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

// NSTC 内层结构是 NamespaceTreeControl → Static → SysTreeView32，展开图标
// 就画在那个 SysTreeView32 上。先直查，再查一层子窗口，兼容不同层级
static HWND InnerTreeView(HWND nstc)
{
    if (HWND t = FindWindowExW(nstc, nullptr, L"SysTreeView32", nullptr)) return t;
    for (HWND c = FindWindowExW(nstc, nullptr, nullptr, nullptr); c;
         c = FindWindowExW(nstc, c, nullptr, nullptr)) {
        if (HWND t = FindWindowExW(c, nullptr, L"SysTreeView32", nullptr)) return t;
    }
    return nullptr;
}

void DirectoryTree::SetClassicExpandIcons(bool classic)
{
    if (classicExpandIcons_ == classic) return;
    classicExpandIcons_ = classic;
    if (!hwnd_ || !IsWindow(hwnd_)) return;

    HWND tree = InnerTreeView(hwnd_);
    if (!tree) {
        WriteAppLog(L"NSTC inner SysTreeView32 not found (SetClassicExpandIcons)");
        return;
    }
    // 与 Q-Dir 同款：给内层 TreeView 换主题即可在两种展开图标间切换——
    //   空主题      → 经典 +/- 方框
    //   "Explorer" → Win7/Vista 三角/箭头
    if (classic)
        SetWindowTheme(tree, L"", L"");
    else
        SetWindowTheme(tree, L"Explorer", nullptr);
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

std::wstring DirectoryTree::SelectedPath()
{
    if (!ctl_) return {};
    // GetSelectedItems 在基接口 INameSpaceTreeControl 上（2 接口只加了 SetOptionsVisible）
    ComPtr<INameSpaceTreeControl> base;
    if (FAILED(ctl_->QueryInterface(IID_PPV_ARGS(&base))) || !base) return {};
    ComPtr<IShellItemArray> arr;
    IShellItem* psi = nullptr;
    std::wstring out;
    if (SUCCEEDED(base->GetSelectedItems(&arr)) && arr &&
        SUCCEEDED(arr->GetItemAt(0, &psi)) && psi) {
        out = PathOfItem(psi);
        psi->Release();
    }
    return out;
}

void DirectoryTree::OnSelectionChanged(IShellItem* psi)
{
    if (!psi) return;
    std::wstring path = PathOfItem(psi);
    // 程序化选中的事件（可能异步到达）按路径计数吞掉，不当作用户导航
    for (auto it = pendingSelects_.begin(); it != pendingSelects_.end(); ++it) {
        if (!path.empty() && _wcsicmp(it->first.c_str(), path.c_str()) == 0) {
            if (--it->second == 0) pendingSelects_.erase(it);
            return;
        }
    }
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
    // 已展开的祖先跳过：重复 SetItemState(EXPANDED) 会惊动控件重摆可见
    // 区域（文件列表导航回同步树时整棵树闪一下）。
    auto expandPrefix = [&](const std::wstring& prefix) {
        UniquePIDL anc = shell::PIDLFromPath(prefix.c_str());
        if (!anc) return;
        ComPtr<IShellItem> ancItem;
        if (SUCCEEDED(SHCreateItemFromIDList(PidlOf(anc), IID_PPV_ARGS(&ancItem))) && ancItem) {
            NSTCITEMSTATE st = NSTCIS_EXPANDED;
            if (SUCCEEDED(ctl_->GetItemState(ancItem.Get(), NSTCIS_EXPANDED, &st)) &&
                (st & NSTCIS_EXPANDED))
                return;
            ctl_->SetItemState(ancItem.Get(), NSTCIS_EXPANDED, NSTCIS_EXPANDED);
        }
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
        // 目标已是选中项：控件不会再发事件，不能记待消费数（残留会误吞
        // 下一次真实用户选中）；此时也不需要滚动（树已经在正确位置，
        // 再 EnsureItemVisible 会触发无谓重绘）。否则先记数再选中。
        std::wstring cur = SelectedPath();
        if (_wcsicmp(cur.c_str(), target.c_str()) == 0) return;
        ++pendingSelects_[target];
        ctl_->SetItemState(item.Get(), NSTCIS_SELECTED, NSTCIS_SELECTED);
        ctl_->EnsureItemVisible(item.Get());
    }
}
