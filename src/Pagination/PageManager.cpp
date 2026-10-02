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

bool PageManager::RequestPage(size_t pageIndex)
{
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
    for (size_t p : { currentPage == 0 ? 0 : currentPage - 1, currentPage + 1 }) {
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
    unsigned long long gen = genAtomic_.load();
    for (;;) {
        size_t page;
        {
            std::unique_lock<std::mutex> lk(cvMtx_);
            cv_.wait(lk, [&] { return stop_ || queuedPage_ != SIZE_MAX; });
            if (stop_) return;
            page = queuedPage_;
            queuedPage_ = SIZE_MAX;
        }

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
