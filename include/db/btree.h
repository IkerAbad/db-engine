#pragma once

#include "db/buffer_pool.h"
#include "db/page.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace db {

// B+tree keyed by a signed 64-bit primary key. Leaves are linked through
// next/prev pointers and every table has a fixed meta page that points at the
// current root and at the first leaf. The stable meta page is what makes root
// splits crash-safe: the root pointer is a single 4 KB page image.
class BTree {
public:
    BTree(BufferPool& pool, PageId meta_page);

    static PageId create(BufferPool& pool, std::uint32_t table_id);

    PageId meta_page() const noexcept { return meta_page_; }
    PageId root() const noexcept { return root_; }
    PageId head() const noexcept { return head_; }

    bool insert(std::int64_t key, std::span<const std::uint8_t> payload);
    bool upsert(std::int64_t key, std::span<const std::uint8_t> payload);
    bool remove(std::int64_t key);
    bool contains(std::int64_t key);
    std::optional<std::vector<std::uint8_t>> find(std::int64_t key);

    // Rebuilds root and internal levels from the leaf chain. Used after a
    // crash, when page writes may have left the internal pointers behind.
    void rebuild();

    bool check(std::string& error);
    std::uint64_t row_count();

    class Cursor {
    public:
        explicit Cursor(BTree& tree);
        ~Cursor();
        Cursor(const Cursor&) = delete;
        Cursor& operator=(const Cursor&) = delete;

        bool seek(std::int64_t key);
        bool seek_le(std::int64_t key);
        bool first();
        bool last();
        bool next();
        bool prev();

        bool valid() const noexcept { return valid_; }
        std::int64_t key() const;
        std::span<const std::uint8_t> payload() const;

    private:
        void release();
        void adopt(PageId leaf, int slot);

        BTree& tree_;
        PageId leaf_ = kInvalidPage;
        int slot_ = 0;
        bool valid_ = false;
    };

private:
    struct SplitResult {
        bool inserted = false;
        bool split = false;
        std::int64_t promo = 0;
        PageId right = kInvalidPage;
    };

    struct RemoveResult {
        bool removed = false;
        bool underflow = false;
        std::optional<std::int64_t> first_key;
    };

    SplitResult insert_rec(PageId pid, std::int64_t key,
                           std::span<const std::uint8_t> payload);
    SplitResult split_leaf(Page& left);
    SplitResult split_internal(Page& page);
    PageId insert_root(PageId root, std::int64_t key,
                       std::span<const std::uint8_t> payload, bool& inserted);

    RemoveResult remove_rec(PageId pid, std::int64_t key, bool is_root);
    void rebalance_child(Page& parent, int child_cell);
    void merge_pages(Page& left, Page& right, Page& parent, int separator_cell);
    void rotate_right_to_left(Page& left, Page& right, Page& parent,
                              int separator_cell);
    void rotate_left_to_right(Page& left, Page& right, Page& parent,
                              int separator_cell);

    void set_root(PageId root);
    std::optional<std::int64_t> first_key_of(Page& page);
    static int child_cell_for(const Page& page, std::int64_t key);
    static PageId child_at(const Page& page, int cell);
    static std::int64_t child_min(const Page& page, int cell);
    static bool can_merge(const Page& left, const Page& right);

    BufferPool& pool_;
    PageId meta_page_ = kInvalidPage;
    PageId root_ = kInvalidPage;
    PageId head_ = kInvalidPage;
};

}  // namespace db
