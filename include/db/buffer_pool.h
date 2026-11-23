#pragma once

#include "db/common.h"
#include "db/file.h"
#include "db/page.h"
#include "db/wal.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace db {

struct BufferPoolStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t flushes = 0;

    double hit_rate() const {
        const std::uint64_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

// Fixed-size LRU buffer pool. Pages are pinned while a caller holds a
// reference and unpinned explicitly. A dirty page is written through the WAL:
// its full image is appended and synced before the data file is touched.
class BufferPool {
public:
    BufferPool(File& data, Wal& wal, std::size_t capacity);

    Page& fetch(PageId id, bool allow_corrupt = false);
    void unpin(PageId id, bool dirty);

    void flush(PageId id);
    void flush_all();

    PageId allocate();
    void set_next_page(PageId next);
    void note_page_seen(PageId id);
    PageId next_page() const noexcept { return next_page_; }

    bool in_memory(PageId id) const;
    std::size_t resident() const noexcept { return used_frames_; }
    std::size_t capacity() const noexcept { return frames_.size(); }
    std::size_t dirty_pages() const;
    const BufferPoolStats& stats() const noexcept { return stats_; }

private:
    struct Frame {
        Page page;
        PageId id = kInvalidPage;
        int pin = 0;
        bool dirty = false;
        bool corrupt = false;
        std::list<std::size_t>::iterator lru_it{};
    };

    void load_page(PageId id, Frame& frame, bool allow_corrupt);
    void write_frame(Frame& frame, bool sync_wal);
    std::size_t pick_frame();
    void touch(Frame& frame);

    File& data_;
    Wal& wal_;
    std::vector<Frame> frames_;
    std::size_t used_frames_ = 0;
    std::unordered_map<PageId, std::size_t> index_;
    std::list<std::size_t> lru_;
    PageId next_page_ = 1;
    BufferPoolStats stats_;
};

}  // namespace db
