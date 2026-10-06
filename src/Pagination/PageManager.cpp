#include "PageManager.h"
#include "../Shell/ShellEnumerator.h"

void PageManager::OpenDirectory(const std::wstring& dir, size_t pageSize)
{
    Shutdown();

    std::lock_guard<std::mutex> lk(mtx_);
    dir_ = dir;
    pageSize_ = pageSize;
    cache_.clear();
    pending_.clear();
    totalCount_ = 0;
    totalCountAtomic_.store(0);
    ++generation_;
    genAtomic_.store(generation_);
    queuedPage_ = 0;

    stop_ = false;
    worker_ = std::thread(&PageManager::WorkerLoop, this);
}

void PageManager::Invalidate()
{
    std::lock_guard<std::mutex> lk(mtx_);
    cache_.clear();
    pending_.clear();
    totalCount_ = 0;
    totalCountAtomic_.store(0);
    // 工作线程保持待命；调用方随后 RequestPage 会重新排队加载
}

bool PageManager::RequestPage(size_t pageIndex){
    std::lock_guard<std::mutex> lk(mtx_);
    if (cache_.count(pageIndex)) return true;
    if (pending_.count(pageIndex)) return false;
    pending_[pageIndex] = true;
    {
        std::lock_guard<std::mutex> cvlk(cvMtx_);
        queuedPage_ = pageIndex;
    }
    cv_.notify_one();
    return false;
}

bool PageManager::TryGetPage(size_t pageIndex, std::vector<FileEntry>& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cache_.find(pageIndex);
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
}

void PageManager::PrefetchAround(size_t currentPage)
{
    size_t pages = PageCount();
    for (size_t p : { currentPage == 0 ? 0 : currentPage - 1, currentPage + 1 }) {
        if (pages > 0 && p >= pages) continue; // 越界页不预取（未分页时只有 1 页）
        std::lock_guard<std::mutex> lk(mtx_);
        if (!cache_.count(p) && !pending_.count(p)) {
            pending_[p] = true;
            std::lock_guard<std::mutex> cvlk(cvMtx_);
            queuedPage_ = p;
            cv_.notify_one();
        }
    }
}

void PageManager::Shutdown()
{
    {
        std::lock_guard<std::mutex> cvlk(cvMtx_);
        stop_ = true;
        ++generation_; // 旧线程结果作废
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(mtx_);
    cache_.clear();
    pending_.clear();
}

void PageManager::WorkerLoop()
{
    for (;;) {
        size_t page = SIZE_MAX;
        {
            // 最新请求优先：用户正在看的页先加载
            std::lock_guard<std::mutex> lk(cvMtx_);
            if (queuedPage_ != SIZE_MAX) {
                page = queuedPage_;
                queuedPage_ = SIZE_MAX;
            }
        }
        if (page == SIZE_MAX) {
            {   // 单槽队列会被后来的请求覆盖：把还挂在 pending_ 里的旧请求
                // 捡回来逐个消化，否则它们永远卡在 pending、页面永远加载不出
                std::lock_guard<std::mutex> lk(mtx_);
                for (const auto& entry : pending_)
                    if (page == SIZE_MAX || entry.first < page) page = entry.first;
            }
            if (page == SIZE_MAX) {
                std::unique_lock<std::mutex> lk(cvMtx_);
                cv_.wait(lk, [&] { return stop_ || queuedPage_ != SIZE_MAX; });
                if (stop_) return;
                page = queuedPage_;
                queuedPage_ = SIZE_MAX;
            }
        }
        if (stop_) return;

        // 后台执行：先取这一页
        std::vector<FileEntry> entries;
        unsigned long long total = 0;
        shell::EnumeratePage(dir_, page * pageSize_, pageSize_, entries,
                             &total);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (total > 0) {
                totalCount_ = total;
                totalCountAtomic_.store(total);
            }
            cache_[page] = std::move(entries);
            pending_.erase(page);
        }
        if (notify_) notify_(); // UI 层负责转到 UI 线程
    }
}
