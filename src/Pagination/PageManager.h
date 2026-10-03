#pragma once
#include "../FileModel/FileEntry.h"
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <condition_variable>

// 目录分页模型：
//   - 打开目录时后台线程统计总数 + 加载第 0 页
//   - 翻页时：缓存命中 -> 立即返回；未命中 -> 后台加载，UI 显示等待
//   - 预取：当前页 ±1 在空闲时提前加载
class PageManager {
public:
    using NotifyFn = std::function<void()>; // 在 UI 线程回调（PostMessage 已由 UI 层处理）

    void SetNotify(NotifyFn fn) { notify_ = std::move(fn); }

    // 打开新目录（重置缓存并后台加载第 0 页）
    void OpenDirectory(const std::wstring& dir, size_t pageSize);

    // 清空缓存强制重新枚举：本程序/外部对当前目录做增删改后调用，
    // 否则 RefreshList 命中旧缓存，列表显示的还是操作前的内容
    void Invalidate();

    // 请求加载某一页。返回 true = 已在缓存（可立即取）；false = 已提交后台加载
    bool RequestPage(size_t pageIndex);

    // 取页内容（仅当缓存命中时填充 out）
    bool TryGetPage(size_t pageIndex, std::vector<FileEntry>& out) const;

    unsigned long long TotalCount() const { return totalCountAtomic_.load(); }
    size_t PageSize() const { return pageSize_; }
    size_t PageCount() const {
        unsigned long long t = totalCountAtomic_.load();
        return static_cast<size_t>((t + pageSize_ - 1) / pageSize_);
    }
    const std::wstring& Directory() const { return dir_; }

    // 空闲时预取 (current ±1)
    void PrefetchAround(size_t currentPage);

    // 关闭目录 / 析构前停止后台线程
    void Shutdown();

    ~PageManager() { Shutdown(); }

private:
    void WorkerLoop();
    void LoadPage(size_t pageIndex); // 后台线程执行

    std::wstring dir_;
    size_t pageSize_ = 100;

    mutable std::mutex mtx_;
    std::map<size_t, std::vector<FileEntry>> cache_;
    std::map<size_t, bool> pending_; // 正在后台加载的页
    unsigned long long totalCount_ = 0; // guarded by mtx_ + atomic mirror
    std::atomic<unsigned long long> totalCountAtomic_{0};

    std::thread worker_;
    std::condition_variable cv_;
    std::mutex cvMtx_;
    bool stop_ = false;
    size_t queuedPage_ = SIZE_MAX; // 待加载页号
    unsigned long long generation_ = 0; // 目录代数，丢弃旧目录的加载结果
    std::atomic<unsigned long long> genAtomic_{0};

    NotifyFn notify_;
};
