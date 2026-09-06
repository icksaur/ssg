#pragma once

// A bounded LRU cache of grapheme-shaped line runs.
//
// A line's CellRun is a pure function of its exact (text, tabWidth), so a cache
// hit is byte-identical to a fresh computeCellRun and the cache
// needs NO semantic invalidation -- only capacity eviction. This is the
// library-only rendering seam for Lever 2's visible-line shaping: the two hot
// visible-line paths (renderFrame and computeUnwrappedViewport)
// re-shape the same on-screen lines every frame during navigation; each holds
// its own cache across frames so a scroll or an in-page caret move reuses the
// unchanged rows instead of re-segmenting them.
//
// Not thread-safe: each render loop and runtime viewport keeps its own instance.
// The cache stores only stable document-line text; synthetic phantom and
// merged-diff runs are shaped fresh by their callers.

#include <ssg/GraphemeLayout.h>

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ssg {

// Capacity is bounded to comfortably exceed a viewport's row count plus scroll
// churn, so the visible working set never evicts itself within a frame.
inline constexpr std::size_t kDefaultLineLayoutCacheCapacity = 512;

class LineLayoutCache {
public:
    explicit LineLayoutCache(
        std::size_t capacity = kDefaultLineLayoutCacheCapacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    // Move-only: the index keys on string_views into the owning list nodes, so a
    // copy would leave the duplicated map referencing the source list. A move
    // transfers the list's address-stable nodes, keeping every view valid.
    LineLayoutCache(const LineLayoutCache&) = delete;
    LineLayoutCache& operator=(const LineLayoutCache&) = delete;
    LineLayoutCache(LineLayoutCache&&) = default;
    LineLayoutCache& operator=(LineLayoutCache&&) = default;

    // The CellRun for (text, tabWidth), served from cache when present and
    // computed (and inserted, evicting the least-recently-used) on a miss. The
    // reference is valid until the next mutating call on this cache; callers
    // consume it before the next `run`.
    const CellRun& run(std::string_view text, int tabWidth) {
        if (auto it = index_.find(Key{text, tabWidth}); it != index_.end()) {
            entries_.splice(entries_.begin(), entries_, it->second);
            return it->second->run;
        }
        entries_.push_front(
            Entry{std::string{text}, tabWidth,
                  computeCellRun(text, tabWidth)});
        index_.emplace(Key{std::string_view{entries_.front().text}, tabWidth},
                       entries_.begin());
        if (entries_.size() > capacity_) {
            const auto& victim = entries_.back();
            index_.erase(Key{std::string_view{victim.text}, victim.tabWidth});
            entries_.pop_back();
        }
        return entries_.front().run;
    }

    void clear() noexcept {
        index_.clear();
        entries_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string text;
        int tabWidth;
        CellRun run;
    };

    // The full (text, tabWidth) key: two tab widths for the same text are
    // distinct entries, so alternating widths coexist rather than evicting each
    // other. The text view points into the owning Entry's std::string.
    struct Key {
        std::string_view text;
        int tabWidth;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return std::hash<std::string_view>{}(key.text) * 1099511628211ULL +
                   static_cast<std::size_t>(key.tabWidth);
        }
    };

    std::size_t capacity_;
    // The list node holds the owning `text`; the map keys on a string_view into
    // that node. std::list nodes are address-stable across splice, so the view
    // stays valid for the entry's lifetime.
    std::list<Entry> entries_;
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
};

}  // namespace ssg
