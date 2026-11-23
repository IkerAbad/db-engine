#include "db/btree.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace db {
namespace {

std::vector<std::uint8_t> make_leaf_cell(std::int64_t key,
                                         std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> cell(10 + payload.size());
    store_i64(cell.data(), key);
    store_u16(cell.data() + 8, static_cast<std::uint16_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(cell.data() + 10, payload.data(), payload.size());
    }
    return cell;
}

std::vector<std::uint8_t> make_internal_cell(std::int64_t key, PageId child) {
    std::vector<std::uint8_t> cell(12);
    store_i64(cell.data(), key);
    store_u32(cell.data() + 8, child);
    return cell;
}

std::span<const std::uint8_t> leaf_payload(const Page& page, int slot) {
    const auto cell = page.cell_at(slot);
    if (cell.size() < 10) {
        fail("leaf cell is truncated");
    }
    const std::uint16_t length = load_u16(cell.data() + 8);
    if (static_cast<std::size_t>(length) + 10 > cell.size()) {
        fail("leaf cell payload is truncated");
    }
    return cell.subspan(10, length);
}

}  // namespace

BTree::BTree(BufferPool& pool, PageId meta_page)
    : pool_(pool), meta_page_(meta_page) {
    Page& meta = pool_.fetch(meta_page);
    if (meta.type() != PageType::Meta ||
        meta.meta_u32(kMetaBodyOffset) != kTableMetaMagic) {
        pool_.unpin(meta_page, false);
        fail("table meta page is invalid");
    }
    root_ = meta.meta_u32(kMetaBodyOffset + 8);
    head_ = meta.meta_u32(kMetaBodyOffset + 12);
    pool_.unpin(meta_page, false);
    if (root_ == kInvalidPage || head_ == kInvalidPage) {
        fail("table meta page has no root");
    }
    pool_.note_page_seen(root_);
    pool_.note_page_seen(head_);
}

PageId BTree::create(BufferPool& pool, std::uint32_t table_id) {
    const PageId meta_page = pool.allocate();
    const PageId root_page = pool.allocate();

    {
        Page& root = pool.fetch(root_page);
        root.init(root_page, PageType::Leaf, 0);
        pool.unpin(root_page, true);
    }
    {
        Page& meta = pool.fetch(meta_page);
        meta.init(meta_page, PageType::Meta, 0);
        meta.set_meta_u32(kMetaBodyOffset, kTableMetaMagic);
        meta.set_meta_u32(kMetaBodyOffset + 4, table_id);
        meta.set_meta_u32(kMetaBodyOffset + 8, root_page);
        meta.set_meta_u32(kMetaBodyOffset + 12, root_page);
        pool.unpin(meta_page, true);
    }
    // The leaf has to be durable before anything can point at it.
    pool.flush(root_page);
    pool.flush(meta_page);
    return meta_page;
}

void BTree::set_root(PageId root) {
    Page& meta = pool_.fetch(meta_page_);
    meta.set_meta_u32(kMetaBodyOffset + 8, root);
    pool_.unpin(meta_page_, true);
    root_ = root;
    pool_.note_page_seen(root);
}

int BTree::child_cell_for(const Page& page, std::int64_t key) {
    const int pos = page.lower_bound(key);
    if (pos < page.slot_count() && page.key_at(pos) == key) {
        return pos;
    }
    return pos - 1;
}

PageId BTree::child_at(const Page& page, int cell) {
    if (cell < 0) {
        return page.left_child();
    }
    const auto bytes = page.cell_at(cell);
    if (bytes.size() < 12) {
        fail("internal cell is truncated");
    }
    return load_u32(bytes.data() + 8);
}

std::int64_t BTree::child_min(const Page& page, int cell) {
    return page.key_at(cell);
}

BTree::SplitResult BTree::split_leaf(Page& left) {
    const int n = left.slot_count();
    if (n < 2) {
        fail("cannot split a leaf with fewer than two cells");
    }
    int split = n / 2;
    if (split <= 0) {
        split = 1;
    }

    std::vector<std::vector<std::uint8_t>> moved;
    moved.reserve(static_cast<std::size_t>(n - split));
    for (int i = split; i < n; ++i) {
        const auto cell = left.cell_at(i);
        moved.emplace_back(cell.begin(), cell.end());
    }

    const PageId right_id = pool_.allocate();
    Page& right = pool_.fetch(right_id);
    right.init(right_id, PageType::Leaf, 0);
    for (const auto& cell : moved) {
        const std::int64_t key = load_i64(cell.data());
        if (!right.insert_cell(key, cell)) {
            pool_.unpin(right_id, true);
            fail("right half of a leaf split does not fit");
        }
    }
    for (int i = n - 1; i >= split; --i) {
        left.remove_at(i);
    }

    right.set_next(left.next());
    right.set_prev(left.id());
    if (left.next() != kInvalidPage) {
        Page& next = pool_.fetch(left.next());
        next.set_prev(right_id);
        pool_.unpin(left.next(), true);
    }
    left.set_next(right_id);

    const std::int64_t promo = right.key_at(0);
    pool_.unpin(right_id, true);
    pool_.flush(right_id);
    return {false, true, promo, right_id};
}

BTree::SplitResult BTree::split_internal(Page& page) {
    const int n = page.slot_count();
    if (n < 1) {
        fail("cannot split an empty internal page");
    }
    const int mid = n / 2;
    const std::int64_t promo = page.key_at(mid);
    const PageId promoted_child = child_at(page, mid);

    std::vector<std::vector<std::uint8_t>> moved;
    moved.reserve(static_cast<std::size_t>(n - mid - 1));
    for (int i = mid + 1; i < n; ++i) {
        const auto cell = page.cell_at(i);
        moved.emplace_back(cell.begin(), cell.end());
    }

    const PageId right_id = pool_.allocate();
    Page& right = pool_.fetch(right_id);
    right.init(right_id, PageType::Internal, page.level());
    right.set_left_child(promoted_child);
    for (const auto& cell : moved) {
        const std::int64_t key = load_i64(cell.data());
        if (!right.insert_cell(key, cell)) {
            pool_.unpin(right_id, true);
            fail("right half of an internal split does not fit");
        }
    }
    for (int i = n - 1; i >= mid; --i) {
        page.remove_at(i);
    }

    pool_.unpin(right_id, true);
    pool_.flush(right_id);
    return {false, true, promo, right_id};
}

BTree::SplitResult BTree::insert_rec(PageId pid, std::int64_t key,
                                     std::span<const std::uint8_t> payload) {
    Page& page = pool_.fetch(pid);

    if (page.type() == PageType::Leaf) {
        if (page.find_key(key) >= 0) {
            pool_.unpin(pid, false);
            return {};
        }
        const auto cell = make_leaf_cell(key, payload);
        if (!page.would_fit(cell.size())) {
            const SplitResult split = split_leaf(page);
            if (key < split.promo) {
                if (!page.insert_cell(key, cell)) {
                    pool_.unpin(pid, true);
                    fail("insert into the left half failed");
                }
                pool_.unpin(pid, true);
            } else {
                pool_.unpin(pid, true);
                Page& right = pool_.fetch(split.right);
                if (!right.insert_cell(key, cell)) {
                    pool_.unpin(split.right, true);
                    fail("insert into the right half failed");
                }
                pool_.unpin(split.right, true);
            }
            return {true, true, split.promo, split.right};
        }
        if (!page.insert_cell(key, cell)) {
            pool_.unpin(pid, false);
            fail("leaf insert failed with free space available");
        }
        pool_.unpin(pid, true);
        return {true, false, 0, kInvalidPage};
    }

    if (page.type() != PageType::Internal) {
        pool_.unpin(pid, false);
        fail("B+tree contains a page of the wrong type");
    }

    const int cell_index = child_cell_for(page, key);
    const PageId child = child_at(page, cell_index);
    const SplitResult child_result = insert_rec(child, key, payload);
    if (!child_result.inserted) {
        pool_.unpin(pid, false);
        return {};
    }
    if (!child_result.split) {
        pool_.unpin(pid, false);
        return {true, false, 0, kInvalidPage};
    }

    const auto separator = make_internal_cell(child_result.promo, child_result.right);
    if (!page.would_fit(separator.size())) {
        const SplitResult split = split_internal(page);
        if (child_result.promo < split.promo) {
            if (!page.insert_cell(child_result.promo, separator)) {
                pool_.unpin(pid, true);
                fail("insert into the left internal half failed");
            }
            pool_.unpin(pid, true);
        } else {
            pool_.unpin(pid, true);
            Page& right = pool_.fetch(split.right);
            if (!right.insert_cell(child_result.promo, separator)) {
                pool_.unpin(split.right, true);
                fail("insert into the right internal half failed");
            }
            pool_.unpin(split.right, true);
        }
        return {true, true, split.promo, split.right};
    }

    if (!page.insert_cell(child_result.promo, separator)) {
        pool_.unpin(pid, false);
        fail("internal insert failed with free space available");
    }
    pool_.unpin(pid, true);
    return {true, false, 0, kInvalidPage};
}

PageId BTree::insert_root(PageId root, std::int64_t key,
                          std::span<const std::uint8_t> payload,
                          bool& inserted) {
    std::uint16_t root_level = 0;
    {
        Page& page = pool_.fetch(root);
        root_level = page.level();
        pool_.unpin(root, false);
    }

    const SplitResult result = insert_rec(root, key, payload);
    inserted = result.inserted;
    if (!result.split) {
        return root;
    }

    const PageId new_root_id = pool_.allocate();
    Page& new_root = pool_.fetch(new_root_id);
    new_root.init(new_root_id, PageType::Internal,
                  static_cast<std::uint16_t>(root_level + 1));
    new_root.set_left_child(root);
    const auto separator = make_internal_cell(result.promo, result.right);
    if (!new_root.insert_cell(result.promo, separator)) {
        pool_.unpin(new_root_id, true);
        fail("new root does not fit its first separator");
    }
    pool_.unpin(new_root_id, true);
    // The new root must be on disk before the meta page points at it.
    pool_.flush(new_root_id);
    return new_root_id;
}

bool BTree::insert(std::int64_t key, std::span<const std::uint8_t> payload) {
    bool inserted = false;
    const PageId new_root = insert_root(root_, key, payload, inserted);
    if (inserted && new_root != root_) {
        set_root(new_root);
    }
    return inserted;
}

bool BTree::upsert(std::int64_t key, std::span<const std::uint8_t> payload) {
    if (contains(key)) {
        remove(key);
    }
    return insert(key, payload);
}

BTree::RemoveResult BTree::remove_rec(PageId pid, std::int64_t key,
                                      bool is_root) {
    Page& page = pool_.fetch(pid);

    if (page.type() == PageType::Leaf) {
        if (!page.remove_key(key)) {
            pool_.unpin(pid, false);
            return {};
        }
        const bool underflow = !is_root && page.used_bytes() < kPageSize / 2;
        std::optional<std::int64_t> first;
        if (page.slot_count() > 0) {
            first = page.key_at(0);
        }
        pool_.unpin(pid, true);
        return {true, underflow, first};
    }

    if (page.type() != PageType::Internal) {
        pool_.unpin(pid, false);
        fail("B+tree contains a page of the wrong type");
    }

    const int cell_index = child_cell_for(page, key);
    const PageId child = child_at(page, cell_index);
    const RemoveResult child_result = remove_rec(child, key, false);
    if (!child_result.removed) {
        pool_.unpin(pid, false);
        return {};
    }

    if (cell_index >= 0 && child_result.first_key.has_value()) {
        page.set_key_at(cell_index, *child_result.first_key);
    }
    if (child_result.underflow) {
        rebalance_child(page, cell_index);
    }

    const bool underflow = !is_root && page.used_bytes() < kPageSize / 2;
    const std::optional<std::int64_t> first = first_key_of(page);
    pool_.unpin(pid, true);
    return {true, underflow, first};
}

bool BTree::remove(std::int64_t key) {
    const RemoveResult result = remove_rec(root_, key, true);
    if (!result.removed) {
        return false;
    }

    Page& root = pool_.fetch(root_);
    if (root.type() == PageType::Internal && root.slot_count() == 0) {
        const PageId child = root.left_child();
        pool_.unpin(root_, false);
        {
            pool_.fetch(child);
            pool_.unpin(child, false);
        }
        pool_.flush(child);
        set_root(child);
    } else {
        pool_.unpin(root_, false);
    }
    return true;
}

bool BTree::can_merge(const Page& left, const Page& right) {
    if (left.type() != right.type()) {
        return false;
    }
    std::size_t extra = right.used_bytes() - Page::kHeaderSize;
    if (right.type() == PageType::Internal) {
        extra += 12;
    }
    return left.used_bytes() + extra <= kPageSize;
}

void BTree::merge_pages(Page& left, Page& right, Page& parent,
                        int separator_cell) {
    const bool internal = left.type() == PageType::Internal;
    std::vector<std::vector<std::uint8_t>> moved;
    moved.reserve(right.slot_count());
    for (int i = 0; i < right.slot_count(); ++i) {
        const auto cell = right.cell_at(i);
        moved.emplace_back(cell.begin(), cell.end());
    }

    if (internal) {
        const std::int64_t separator = parent.key_at(separator_cell);
        const PageId right_left = right.left_child();
        const auto bridge = make_internal_cell(separator, right_left);
        if (!left.insert_cell(separator, bridge)) {
            fail("internal merge does not fit");
        }
    }
    for (const auto& cell : moved) {
        const std::int64_t key = load_i64(cell.data());
        if (!left.insert_cell(key, cell)) {
            fail("page merge does not fit");
        }
    }

    if (!internal) {
        left.set_next(right.next());
        if (right.next() != kInvalidPage) {
            Page& next = pool_.fetch(right.next());
            next.set_prev(left.id());
            pool_.unpin(right.next(), true);
        }
    }
    parent.remove_at(separator_cell);
}

void BTree::rotate_right_to_left(Page& left, Page& right, Page& parent,
                                 int separator_cell) {
    if (left.type() == PageType::Leaf) {
        while (left.used_bytes() < kPageSize / 2 && right.slot_count() > 1) {
            const std::int64_t key = right.key_at(0);
            const auto cell = right.cell_at(0);
            if (!left.insert_cell(key, cell)) {
                break;
            }
            right.remove_at(0);
            parent.set_key_at(separator_cell, right.key_at(0));
        }
        return;
    }

    while (left.used_bytes() < kPageSize / 2 && right.slot_count() > 0) {
        const std::int64_t separator = parent.key_at(separator_cell);
        const PageId child = right.left_child();
        const auto bridge = make_internal_cell(separator, child);
        if (!left.insert_cell(separator, bridge)) {
            break;
        }
        const std::int64_t new_separator = right.key_at(0);
        const PageId new_left_child = child_at(right, 0);
        parent.set_key_at(separator_cell, new_separator);
        right.set_left_child(new_left_child);
        right.remove_at(0);
    }
}

void BTree::rotate_left_to_right(Page& left, Page& right, Page& parent,
                                 int separator_cell) {
    if (left.type() == PageType::Leaf) {
        while (right.used_bytes() < kPageSize / 2 && left.slot_count() > 1) {
            const int last = left.slot_count() - 1;
            const std::int64_t key = left.key_at(last);
            const auto cell = left.cell_at(last);
            if (!right.insert_cell(key, cell)) {
                break;
            }
            left.remove_at(last);
            parent.set_key_at(separator_cell, right.key_at(0));
        }
        return;
    }

    while (right.used_bytes() < kPageSize / 2 && left.slot_count() > 0) {
        const int last = left.slot_count() - 1;
        const std::int64_t key = left.key_at(last);
        const PageId child = child_at(left, last);
        const std::int64_t separator = parent.key_at(separator_cell);
        const PageId right_left = right.left_child();
        const auto bridge = make_internal_cell(separator, right_left);
        if (!right.insert_cell(separator, bridge)) {
            break;
        }
        right.set_left_child(child);
        left.remove_at(last);
        parent.set_key_at(separator_cell, key);
    }
}

void BTree::rebalance_child(Page& parent, int child_cell) {
    const int n = parent.slot_count();
    if (n == 0) {
        return;
    }

    PageId left_id = kInvalidPage;
    PageId right_id = kInvalidPage;
    int separator_cell = -1;
    bool child_is_left = false;

    if (child_cell < 0) {
        left_id = parent.left_child();
        right_id = child_at(parent, 0);
        separator_cell = 0;
        child_is_left = true;
    } else if (child_cell + 1 < n) {
        left_id = child_at(parent, child_cell);
        right_id = child_at(parent, child_cell + 1);
        separator_cell = child_cell + 1;
        child_is_left = true;
    } else if (child_cell == 0) {
        left_id = parent.left_child();
        right_id = child_at(parent, 0);
        separator_cell = 0;
        child_is_left = false;
    } else {
        left_id = child_at(parent, child_cell - 1);
        right_id = child_at(parent, child_cell);
        separator_cell = child_cell;
        child_is_left = false;
    }

    Page& left = pool_.fetch(left_id);
    Page& right = pool_.fetch(right_id);
    if (left.type() != right.type()) {
        pool_.unpin(left_id, false);
        pool_.unpin(right_id, false);
        fail("sibling pages have different types");
    }

    const bool other_is_rich =
        (child_is_left ? right.used_bytes() : left.used_bytes()) > kPageSize / 2;
    const bool mergeable = can_merge(left, right);

    if (mergeable && !other_is_rich) {
        merge_pages(left, right, parent, separator_cell);
    } else if (other_is_rich) {
        if (child_is_left) {
            rotate_right_to_left(left, right, parent, separator_cell);
        } else {
            rotate_left_to_right(left, right, parent, separator_cell);
        }
    } else if (mergeable) {
        merge_pages(left, right, parent, separator_cell);
    } else if (child_is_left && right.slot_count() > 0) {
        rotate_right_to_left(left, right, parent, separator_cell);
    } else if (!child_is_left && left.slot_count() > 1) {
        rotate_left_to_right(left, right, parent, separator_cell);
    }

    pool_.unpin(left_id, true);
    pool_.unpin(right_id, true);
}

std::optional<std::int64_t> BTree::first_key_of(Page& page) {
    if (page.type() == PageType::Leaf) {
        if (page.slot_count() == 0) {
            return std::nullopt;
        }
        return page.key_at(0);
    }
    PageId child = page.left_child();
    int guard = 0;
    while (child != kInvalidPage && guard++ < 1024) {
        Page& next = pool_.fetch(child);
        if (next.type() == PageType::Leaf) {
            std::optional<std::int64_t> key;
            if (next.slot_count() > 0) {
                key = next.key_at(0);
            }
            pool_.unpin(child, false);
            return key;
        }
        const PageId grandchild = next.left_child();
        pool_.unpin(child, false);
        child = grandchild;
    }
    return std::nullopt;
}

bool BTree::contains(std::int64_t key) {
    Cursor cursor(*this);
    return cursor.seek(key) && cursor.key() == key;
}

std::optional<std::vector<std::uint8_t>> BTree::find(std::int64_t key) {
    Cursor cursor(*this);
    if (!cursor.seek(key) || cursor.key() != key) {
        return std::nullopt;
    }
    const auto payload = cursor.payload();
    return std::vector<std::uint8_t>(payload.begin(), payload.end());
}

void BTree::rebuild() {
    const PageId old_head = head_;
    PageId new_root = pool_.allocate();
    if (new_root == old_head) {
        fail("page allocator reused the leaf head during rebuild");
    }
    {
        Page& page = pool_.fetch(new_root);
        page.init(new_root, PageType::Leaf, 0);
        pool_.unpin(new_root, true);
    }

    std::unordered_set<PageId> visited;
    PageId current = old_head;
    while (current != kInvalidPage) {        if (!visited.insert(current).second) {
            fail("leaf chain contains a cycle");
        }
        if (visited.size() > 100'000'000) {
            fail("leaf chain is implausibly long");
        }
        Page& page = pool_.fetch(current);
        if (page.type() != PageType::Leaf) {
            pool_.unpin(current, false);
            fail("leaf chain contains a non-leaf page");
        }
        std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> rows;
        rows.reserve(page.slot_count());
        for (int i = 0; i < page.slot_count(); ++i) {
            const auto payload = leaf_payload(page, i);
            rows.emplace_back(page.key_at(i),
                              std::vector<std::uint8_t>(payload.begin(), payload.end()));
        }
        const PageId next = page.next();
        pool_.unpin(current, false);

        for (const auto& [key, row] : rows) {
            bool inserted = false;
            new_root = insert_root(new_root, key, row, inserted);
        }
        current = next;
    }

    PageId new_head = new_root;
    int guard = 0;
    while (guard++ < 1024) {
        Page& page = pool_.fetch(new_head);
        if (page.type() == PageType::Leaf) {
            pool_.unpin(new_head, false);
            break;
        }
        const PageId child = page.left_child();
        pool_.unpin(new_head, false);
        new_head = child;
    }

    {
        pool_.fetch(new_head);
        pool_.unpin(new_head, false);
    }
    // Every new leaf has to be durable before the meta page points at the new
    // tree. flush_all writes the old meta too, which is a valid fallback.
    pool_.flush_all();

    {
        Page& meta = pool_.fetch(meta_page_);
        meta.set_meta_u32(kMetaBodyOffset + 8, new_root);
        meta.set_meta_u32(kMetaBodyOffset + 12, new_head);
        pool_.unpin(meta_page_, true);
    }
    pool_.flush(meta_page_);
    root_ = new_root;
    head_ = new_head;
    pool_.note_page_seen(new_root);
    pool_.note_page_seen(new_head);
}

bool BTree::check(std::string& error) {
    struct Info {
        bool has = false;
        std::int64_t first = 0;
        std::int64_t last = 0;
        std::uint64_t count = 0;
    };

    error.clear();
    std::vector<PageId> leaves;
    std::uint64_t chain_count = 0;

    std::uint16_t root_level = 0;
    {
        Page& root = pool_.fetch(root_);
        root_level = root.level();
        pool_.unpin(root_, false);
    }

    std::function<bool(PageId, std::uint16_t, Info&)> visit =
        [&](PageId id, std::uint16_t expected_level, Info& info) -> bool {
        Page& page = pool_.fetch(id);
        const auto finish = [&](bool ok) {
            pool_.unpin(id, false);
            return ok;
        };

        if (page.type() == PageType::Leaf) {
            if (expected_level != 0) {
                error = "leaf at level " + std::to_string(expected_level);
                return finish(false);
            }
            for (int i = 0; i < page.slot_count(); ++i) {
                if (i > 0 && page.key_at(i) <= page.key_at(i - 1)) {
                    error = "leaf keys are not strictly increasing";
                    return finish(false);
                }
            }
            leaves.push_back(id);
            if (page.slot_count() > 0) {
                info.has = true;
                info.first = page.key_at(0);
                info.last = page.key_at(page.slot_count() - 1);
                info.count = static_cast<std::uint64_t>(page.slot_count());
                chain_count += info.count;
            }
            return finish(true);
        }

        if (page.type() != PageType::Internal) {
            error = "page " + std::to_string(id) + " has an invalid type";
            return finish(false);
        }
        if (page.level() != expected_level || page.level() == 0) {
            error = "internal page at the wrong level";
            return finish(false);
        }
        if (page.slot_count() == 0 || page.left_child() == kInvalidPage) {
            error = "internal page has no children";
            return finish(false);
        }

        const int n = page.slot_count();
        std::optional<std::int64_t> previous_separator;
        Info left_info;
        if (!visit(page.left_child(),
                   static_cast<std::uint16_t>(expected_level - 1), left_info)) {
            return finish(false);
        }
        if (left_info.has) {
            info.has = true;
            info.first = left_info.first;
            info.count += left_info.count;
        }
        for (int i = 0; i < n; ++i) {
            const std::int64_t separator = page.key_at(i);
            if (previous_separator && separator <= *previous_separator) {
                error = "internal separators are not increasing";
                return finish(false);
            }
            if (left_info.has && left_info.last >= separator) {
                error = "left child overlaps a separator";
                return finish(false);
            }
            Info child_info;
            if (!visit(child_at(page, i),
                       static_cast<std::uint16_t>(expected_level - 1),
                       child_info)) {
                return finish(false);
            }
            if (!child_info.has || child_info.first != separator) {
                error = "separator does not match the child minimum";
                return finish(false);
            }
            if (!info.has) {
                info.has = true;
                info.first = child_info.first;
            }
            info.last = child_info.last;
            info.count += child_info.count;
            previous_separator = separator;
            left_info = child_info;
        }
        return finish(true);
    };

    Info root_info;
    if (!visit(root_, root_level, root_info)) {
        return false;
    }

    if (leaves.empty()) {
        error = "tree has no leaves";
        return false;
    }
    if (leaves.front() != head_) {
        error = "meta head does not point at the first leaf";
        return false;
    }
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        Page& leaf = pool_.fetch(leaves[i]);
        const PageId next = leaf.next();
        const PageId prev = leaf.prev();
        pool_.unpin(leaves[i], false);
        if (i + 1 < leaves.size()) {
            if (next != leaves[i + 1]) {
                error = "leaf next link is broken";
                return false;
            }
        } else if (next != kInvalidPage) {
            error = "last leaf has a next link";
            return false;
        }
        if (i == 0) {
            if (prev != kInvalidPage) {
                error = "first leaf has a prev link";
                return false;
            }
        } else if (prev != leaves[i - 1]) {
            error = "leaf prev link is broken";
            return false;
        }
    }
    if (root_info.count != chain_count) {
        error = "row count differs from the leaf chain";
        return false;
    }
    return true;
}

std::uint64_t BTree::row_count() {
    std::uint64_t count = 0;
    PageId current = head_;
    int guard = 0;
    while (current != kInvalidPage && guard++ < 100'000'000) {
        Page& page = pool_.fetch(current);
        count += page.slot_count();
        const PageId next = page.next();
        pool_.unpin(current, false);
        current = next;
    }
    return count;
}

BTree::Cursor::Cursor(BTree& tree) : tree_(tree) {}

BTree::Cursor::~Cursor() {
    release();
}

void BTree::Cursor::release() {
    if (leaf_ != kInvalidPage) {
        tree_.pool_.unpin(leaf_, false);
        leaf_ = kInvalidPage;
    }
    valid_ = false;
}

void BTree::Cursor::adopt(PageId leaf, int slot) {
    if (leaf == kInvalidPage) {
        release();
        return;
    }
    if (leaf == leaf_) {
        slot_ = slot;
        valid_ = true;
        return;
    }
    release();
    Page& page = tree_.pool_.fetch(leaf);
    if (page.type() != PageType::Leaf) {
        tree_.pool_.unpin(leaf, false);
        fail("cursor reached a non-leaf page");
    }
    leaf_ = leaf;
    slot_ = slot;
    valid_ = true;
}

bool BTree::Cursor::seek(std::int64_t key) {
    release();
    PageId current = tree_.root_;
    int guard = 0;
    while (guard++ < 1024) {
        Page& page = tree_.pool_.fetch(current);
        if (page.type() == PageType::Leaf) {
            const int pos = page.lower_bound(key);
            const int count = page.slot_count();
            const PageId next = page.next();
            tree_.pool_.unpin(current, false);
            if (pos < count) {
                adopt(current, pos);
                return true;
            }
            PageId leaf = next;
            int steps = 0;
            while (leaf != kInvalidPage && steps++ < 1024) {
                Page& candidate = tree_.pool_.fetch(leaf);
                const int candidate_count = candidate.slot_count();
                const PageId following = candidate.next();
                tree_.pool_.unpin(leaf, false);
                if (candidate_count > 0) {
                    adopt(leaf, 0);
                    return true;
                }
                leaf = following;
            }
            return false;
        }
        const int cell = child_cell_for(page, key);
        const PageId child = child_at(page, cell);
        tree_.pool_.unpin(current, false);
        current = child;
    }
    fail("B+tree is too deep");
}

bool BTree::Cursor::seek_le(std::int64_t key) {
    release();
    PageId current = tree_.root_;
    int guard = 0;
    while (guard++ < 1024) {
        Page& page = tree_.pool_.fetch(current);
        if (page.type() == PageType::Leaf) {
            const int count = page.slot_count();
            int pos = page.lower_bound(key);
            if (pos >= count || page.key_at(pos) != key) {
                --pos;
            }
            const PageId previous = page.prev();
            tree_.pool_.unpin(current, false);
            if (pos >= 0) {
                adopt(current, pos);
                return true;
            }
            PageId leaf = previous;
            int steps = 0;
            while (leaf != kInvalidPage && steps++ < 1024) {
                Page& candidate = tree_.pool_.fetch(leaf);
                const int candidate_count = candidate.slot_count();
                const PageId before = candidate.prev();
                tree_.pool_.unpin(leaf, false);
                if (candidate_count > 0) {
                    adopt(leaf, candidate_count - 1);
                    return true;
                }
                leaf = before;
            }
            return false;
        }
        const int cell = child_cell_for(page, key);
        const PageId child = child_at(page, cell);
        tree_.pool_.unpin(current, false);
        current = child;
    }
    fail("B+tree is too deep");
}

bool BTree::Cursor::first() {
    release();
    PageId current = tree_.root_;
    int guard = 0;
    while (guard++ < 1024) {
        Page& page = tree_.pool_.fetch(current);
        if (page.type() == PageType::Leaf) {
            const int count = page.slot_count();
            const PageId next = page.next();
            tree_.pool_.unpin(current, false);
            if (count > 0) {
                adopt(current, 0);
                return true;
            }
            PageId leaf = next;
            int steps = 0;
            while (leaf != kInvalidPage && steps++ < 1024) {
                Page& candidate = tree_.pool_.fetch(leaf);
                const int candidate_count = candidate.slot_count();
                const PageId following = candidate.next();
                tree_.pool_.unpin(leaf, false);
                if (candidate_count > 0) {
                    adopt(leaf, 0);
                    return true;
                }
                leaf = following;
            }
            return false;
        }
        const PageId child = page.left_child();
        tree_.pool_.unpin(current, false);
        current = child;
    }
    fail("B+tree is too deep");
}

bool BTree::Cursor::last() {
    release();
    PageId current = tree_.root_;
    int guard = 0;
    while (guard++ < 1024) {
        Page& page = tree_.pool_.fetch(current);
        if (page.type() == PageType::Leaf) {
            const int count = page.slot_count();
            const PageId previous = page.prev();
            tree_.pool_.unpin(current, false);
            if (count > 0) {
                adopt(current, count - 1);
                return true;
            }
            PageId leaf = previous;
            int steps = 0;
            while (leaf != kInvalidPage && steps++ < 1024) {
                Page& candidate = tree_.pool_.fetch(leaf);
                const int candidate_count = candidate.slot_count();
                const PageId before = candidate.prev();
                tree_.pool_.unpin(leaf, false);
                if (candidate_count > 0) {
                    adopt(leaf, candidate_count - 1);
                    return true;
                }
                leaf = before;
            }
            return false;
        }
        const int n = page.slot_count();
        const PageId child = n == 0 ? page.left_child() : child_at(page, n - 1);
        tree_.pool_.unpin(current, false);
        current = child;
    }
    fail("B+tree is too deep");
}

bool BTree::Cursor::next() {
    if (!valid_) {
        return false;
    }
    Page& page = tree_.pool_.fetch(leaf_);
    const int count = page.slot_count();
    const PageId next_leaf = page.next();
    tree_.pool_.unpin(leaf_, false);

    if (slot_ + 1 < count) {
        ++slot_;
        return true;
    }
    PageId leaf = next_leaf;
    int steps = 0;
    while (leaf != kInvalidPage && steps++ < 1024) {
        Page& candidate = tree_.pool_.fetch(leaf);
        const int candidate_count = candidate.slot_count();
        const PageId following = candidate.next();
        tree_.pool_.unpin(leaf, false);
        if (candidate_count > 0) {
            adopt(leaf, 0);
            return true;
        }
        leaf = following;
    }
    release();
    return false;
}

bool BTree::Cursor::prev() {
    if (!valid_) {
        return false;
    }
    if (slot_ > 0) {
        --slot_;
        return true;
    }
    Page& page = tree_.pool_.fetch(leaf_);
    const PageId previous = page.prev();
    tree_.pool_.unpin(leaf_, false);
    PageId leaf = previous;
    int steps = 0;
    while (leaf != kInvalidPage && steps++ < 1024) {
        Page& candidate = tree_.pool_.fetch(leaf);
        const int candidate_count = candidate.slot_count();
        const PageId before = candidate.prev();
        tree_.pool_.unpin(leaf, false);
        if (candidate_count > 0) {
            adopt(leaf, candidate_count - 1);
            return true;
        }
        leaf = before;
    }
    release();
    return false;
}

std::int64_t BTree::Cursor::key() const {
    if (!valid_) {
        fail("cursor is not positioned");
    }
    Page& page = tree_.pool_.fetch(leaf_);
    const std::int64_t value = page.key_at(slot_);
    tree_.pool_.unpin(leaf_, false);
    return value;
}

std::span<const std::uint8_t> BTree::Cursor::payload() const {
    if (!valid_) {
        fail("cursor is not positioned");
    }
    Page& page = tree_.pool_.fetch(leaf_);
    const auto bytes = leaf_payload(page, slot_);
    tree_.pool_.unpin(leaf_, false);
    return bytes;
}

}  // namespace db
