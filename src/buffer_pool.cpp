#include "db/buffer_pool.h"

#include "db/fault.h"

#include <algorithm>
#include <cstdlib>

namespace db {

BufferPool::BufferPool(File& data, Wal& wal, std::size_t capacity)
    : data_(data), wal_(wal), frames_(capacity) {
    if (capacity < 2) {
        fail("buffer pool needs at least two frames");
    }
}

void BufferPool::touch(Frame& frame) {
    lru_.erase(frame.lru_it);
    lru_.push_back(
        static_cast<std::size_t>(&frame - frames_.data()));
    frame.lru_it = std::prev(lru_.end());
}

Page& BufferPool::fetch(PageId id, bool allow_corrupt) {
    if (id == kInvalidPage) {
        fail("fetch of an invalid page id");
    }
    const auto it = index_.find(id);
    if (it != index_.end()) {
        Frame& frame = frames_[it->second];
        ++stats_.hits;
        ++frame.pin;
        touch(frame);
        return frame.page;
    }

    ++stats_.misses;
    const std::size_t idx = pick_frame();
    Frame& frame = frames_[idx];
    frame.pin = 1;
    frame.dirty = false;
    frame.corrupt = false;
    frame.id = id;
    load_page(id, frame, allow_corrupt);
    index_[id] = idx;
    lru_.push_back(idx);
    frame.lru_it = std::prev(lru_.end());
    return frame.page;
}

void BufferPool::load_page(PageId id, Frame& frame, bool allow_corrupt) {
    frame.page.clear();
    const std::uint64_t offset = static_cast<std::uint64_t>(id) * kPageSize;
    if (offset + kPageSize <= data_.size()) {
        data_.read_at(offset, frame.page.raw(), kPageSize);
        if (!frame.page.verify_checksum() || frame.page.id() != id) {
            if (!allow_corrupt) {
                fail("page " + std::to_string(id) + " failed its checksum");
            }
            frame.corrupt = true;
            frame.page.clear();
        }
    }
    ++stats_.reads;
}

std::size_t BufferPool::pick_frame() {
    if (used_frames_ < frames_.size()) {
        return used_frames_++;
    }
    for (const std::size_t idx : lru_) {
        Frame& frame = frames_[idx];
        if (frame.pin == 0) {
            if (frame.dirty) {
                flush(frame.id);
            }
            index_.erase(frame.id);
            frame.id = kInvalidPage;
            frame.dirty = false;
            frame.corrupt = false;
            lru_.erase(frame.lru_it);
            ++stats_.evictions;
            return idx;
        }
    }
    fail("buffer pool is exhausted: every page is pinned");
}

void BufferPool::unpin(PageId id, bool dirty) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        fail("unpin of a page that is not in the pool");
    }
    Frame& frame = frames_[it->second];
    if (frame.pin <= 0) {
        fail("unpin of an already unpinned page");
    }
    frame.pin--;
    frame.dirty = frame.dirty || dirty;
    if (frame.pin == 0) {
        touch(frame);
    }
}

void BufferPool::write_frame(Frame& frame, bool sync_wal) {
    const Lsn lsn = wal_.next_lsn();
    frame.page.set_lsn(lsn);
    frame.page.update_checksum();

    fault::hit("page.write.before_wal");
    wal_.append(RecordType::PageImage, kNoTxn, frame.id, 0,
                static_cast<std::int64_t>(lsn),
                std::span<const std::uint8_t>(frame.page.raw(), kPageSize));
    fault::hit("page.write.after_wal");
    if (sync_wal) {
        wal_.sync();
    }
    fault::hit("page.write.before_data");

    const std::uint64_t offset = static_cast<std::uint64_t>(frame.id) * kPageSize;
    if (fault::enabled("page.write.half")) {
        data_.write_at(offset, frame.page.raw(), kPageSize / 2);
        data_.sync();
        std::_Exit(86);
    }
    data_.write_at(offset, frame.page.raw(), kPageSize);
    frame.dirty = false;
    ++stats_.writes;
    fault::hit("page.write.after");
}

void BufferPool::flush(PageId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        fail("flush of a page that is not in the pool");
    }
    Frame& frame = frames_[it->second];
    if (!frame.dirty) {
        return;
    }
    write_frame(frame, true);
}

void BufferPool::flush_all() {
    fault::hit("flush.all.before");
    std::vector<std::size_t> dirty;
    for (std::size_t i = 0; i < used_frames_; ++i) {
        if (frames_[i].dirty) {
            dirty.push_back(i);
        }
    }
    if (dirty.empty()) {
        data_.sync();
        return;
    }

    for (const std::size_t idx : dirty) {
        Frame& frame = frames_[idx];
        const Lsn lsn = wal_.next_lsn();
        frame.page.set_lsn(lsn);
        frame.page.update_checksum();
        wal_.append(RecordType::PageImage, kNoTxn, frame.id, 0,
                    static_cast<std::int64_t>(lsn),
                    std::span<const std::uint8_t>(frame.page.raw(), kPageSize));
    }
    wal_.sync();
    fault::hit("flush.all.after_wal");

    bool half_done = false;
    for (const std::size_t idx : dirty) {
        Frame& frame = frames_[idx];
        const std::uint64_t offset = static_cast<std::uint64_t>(frame.id) * kPageSize;
        if (!half_done && fault::enabled("flush.half_write")) {
            data_.write_at(offset, frame.page.raw(), kPageSize / 2);
            data_.sync();
            std::_Exit(86);
        }
        data_.write_at(offset, frame.page.raw(), kPageSize);
        frame.dirty = false;
        ++stats_.writes;
        half_done = true;
    }
    data_.sync();
    ++stats_.flushes;
    fault::hit("flush.all.after");
}

PageId BufferPool::allocate() {
    Page& meta = fetch(0);
    PageId next = meta.meta_u32(kMetaBodyOffset + 4);
    if (next < next_page_) {
        // The header can lag behind pages that a crash left in the file.
        next = next_page_;
    }
    if (next < 1 || next >= kInvalidPage - 1) {
        unpin(0, false);
        fail("page id space exhausted");
    }
    meta.set_meta_u32(kMetaBodyOffset + 4, next + 1);
    unpin(0, true);
    next_page_ = next + 1;
    return next;
}

void BufferPool::set_next_page(PageId next) {
    if (next > next_page_) {
        next_page_ = next;
    }
}

void BufferPool::note_page_seen(PageId id) {
    if (id != kInvalidPage && id + 1 > next_page_) {
        next_page_ = id + 1;
    }
}

bool BufferPool::in_memory(PageId id) const {
    return index_.contains(id);
}

std::size_t BufferPool::dirty_pages() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < used_frames_; ++i) {
        if (frames_[i].dirty) {
            ++count;
        }
    }
    return count;
}

}  // namespace db
