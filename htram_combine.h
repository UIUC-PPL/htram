#ifndef __HTRAM_COMBINE_H__
#define __HTRAM_COMBINE_H__
//
// CombiningHold: the per-destination set of items a source is holding before
// they leave, keyed so that a second item for the same key folds into the
// first instead of travelling separately.
//
// Byte-oriented on purpose. The rest of htram is still compiled against one
// payload type, but the SC27 plan's step 8 erases that type, and a hold
// written against `datatype` would have to be rewritten during the refactor.
// Everything here sees an item as item_size bytes plus the operations in
// HoldOps, so step 8 lifts it unchanged.
//
// Structure, per destination:
//
//   entries   a pool of fixed-size records: key, bucket, stamp, payload bytes.
//             Indices are stable, so nothing that refers to an entry moves
//             when the table below is rebuilt.
//   table     open-addressed, power-of-two, linear probing, holding entry
//             indices, with an 8-bit tag per slot so a negative lookup almost
//             never touches an entry.
//   lists     one vector per priority bucket of (entry, stamp) references.
//
// Decrease-key is lazy. When a fold moves an entry to a better bucket, the
// entry's bucket and stamp change and a new reference is pushed onto the new
// bucket's list; the old reference is left behind and skipped at release
// because its stamp no longer matches. O(1) and no allocation on the path
// that matters, at the price of stale references that are reclaimed when
// their bucket is drained.
//
// Exact live counts per bucket are kept alongside, because the library's
// release trigger is a count of admitted items, and the lists' lengths
// include the stale references.
//
#include <cstdint>
#include <cstring>
#include <vector>

struct HoldOps {
  size_t item_size;
  // The combining key: two items with the same key are one item.
  uint64_t (*key)(const void *item);
  // Fold `incoming` into `held`, leaving the survivor in `held` and a copy of
  // whatever no longer exists in `retired`. Returns true if the survivor now
  // carries the incoming item's priority bucket. For a min-combine that is
  // "incoming was better": held takes it, and the old held item is retired.
  bool (*combine)(void *held, const void *incoming, void *retired);
  // The application's bookkeeping for an item that will never be delivered.
  // A correctness requirement, not a statistic: SSSP counts every item it
  // creates and terminates when every one has been processed, so an item
  // destroyed without this call makes the run hang.
  void (*on_absorb)(void *client, const void *retired);
};

class CombiningHold {
public:
  struct InsertResult {
    bool absorbed;   // folded into an existing entry
    int old_bucket;  // the entry's bucket before the fold, or -1 if inserted
    int new_bucket;  // the bucket the item now lives in
  };

  CombiningHold() {}

  void init(const HoldOps *ops, void *client, int buckets) {
    ops_ = ops;
    client_ = client;
    size_ = ops->item_size;
    lists_.assign((size_t)buckets, std::vector<Ref>());
    live_.assign((size_t)buckets, 0);
    lowest_ = buckets;
    rebuild(64);
    retired_.assign(size_, 0);
  }

  // Add an item at `bucket`, or fold it into the entry already held for its
  // key. on_absorb has run for the retired item before this returns.
  InsertResult insert(const void *item, int bucket) {
    uint64_t key = ops_->key(item);
    uint64_t h = mix(key);
    uint8_t tag = tagOf(h);
    size_t slot = (size_t)h & mask_;
    while (slots_[slot]) {
      uint32_t e = slots_[slot] - 1;
      if (tags_[slot] == tag && keys_[e] == key) {
        InsertResult r;
        r.absorbed = true;
        r.old_bucket = buckets_[e];
        bool moved = ops_->combine(payload(e), item, retired_.data());
        ops_->on_absorb(client_, retired_.data());
        absorbed_++;
        if (moved && bucket != buckets_[e]) {
          live_[(size_t)buckets_[e]]--;
          buckets_[e] = bucket;
          live_[(size_t)bucket]++;
          stamps_[e]++;
          lists_[(size_t)bucket].push_back(Ref{e, stamps_[e]});
          if (bucket < lowest_)
            lowest_ = bucket;
        }
        r.new_bucket = buckets_[e];
        return r;
      }
      slot = (slot + 1) & mask_;
    }
    uint32_t e = allocEntry();
    keys_[e] = key;
    buckets_[e] = bucket;
    stamps_[e]++;
    std::memcpy(payload(e), item, size_);
    slots_[slot] = e + 1;
    tags_[slot] = tag;
    count_++;
    live_[(size_t)bucket]++;
    lists_[(size_t)bucket].push_back(Ref{e, stamps_[e]});
    if (bucket < lowest_)
      lowest_ = bucket;
    if (count_ * 2 > mask_ + 1)
      rebuild((mask_ + 1) * 2);
    inserted_++;
    InsertResult r;
    r.absorbed = false;
    r.old_bucket = -1;
    r.new_bucket = bucket;
    return r;
  }

  // Remove up to `limit` items from buckets [lowest, high], lowest first,
  // passing each to emit(ctx, item_bytes). Returns how many were removed.
  template <typename Emit>
  long release(int high, long limit, Emit &&emit) {
    long removed = 0;
    int last = (int)lists_.size() - 1;
    if (high > last)
      high = last;
    for (int b = lowest_; b <= high && removed < limit; b++) {
      std::vector<Ref> &list = lists_[(size_t)b];
      size_t i = 0;
      for (; i < list.size() && removed < limit; i++) {
        uint32_t e = list[i].entry;
        if (stamps_[e] != list[i].stamp)
          continue; // moved to another bucket, or already released
        emit(payload(e));
        erase(e);
        live_[(size_t)b]--;
        removed++;
      }
      if (i == list.size())
        list.clear();
      else
        list.erase(list.begin(), list.begin() + (long)i);
    }
    while (lowest_ < (int)live_.size() && live_[(size_t)lowest_] == 0) {
      lists_[(size_t)lowest_].clear(); // only stale references can remain
      lowest_++;
    }
    return removed;
  }

  // Live items in bucket b: exact, unlike the list length.
  long live(int b) const { return live_[(size_t)b]; }
  long size() const { return (long)count_; }
  unsigned long long absorbed() const { return absorbed_; }
  unsigned long long inserted() const { return inserted_; }

private:
  struct Ref {
    uint32_t entry;
    uint32_t stamp;
  };

  static uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }
  // The slot index uses the low bits, so the tag comes from the high ones.
  static uint8_t tagOf(uint64_t h) { return (uint8_t)(h >> 56); }

  void *payload(uint32_t e) { return &payloads_[(size_t)e * size_]; }

  uint32_t allocEntry() {
    if (!free_.empty()) {
      uint32_t e = free_.back();
      free_.pop_back();
      return e;
    }
    uint32_t e = (uint32_t)keys_.size();
    keys_.push_back(0);
    buckets_.push_back(0);
    stamps_.push_back(0);
    payloads_.resize(payloads_.size() + size_);
    return e;
  }

  // Remove entry e from the table with backward-shift deletion, which keeps
  // every probe sequence unbroken without tombstones, and return it to the
  // pool. Bumping the stamp invalidates any reference still naming it.
  void erase(uint32_t e) {
    uint64_t key = keys_[e];
    size_t slot = (size_t)mix(key) & mask_;
    while (slots_[slot] != e + 1)
      slot = (slot + 1) & mask_;
    size_t hole = slot;
    size_t next = (hole + 1) & mask_;
    while (slots_[next]) {
      size_t home = (size_t)mix(keys_[slots_[next] - 1]) & mask_;
      // Move next into the hole if its home does not lie cyclically in
      // (hole, next].
      bool movable = (next > hole) ? (home <= hole || home > next)
                                   : (home <= hole && home > next);
      if (movable) {
        slots_[hole] = slots_[next];
        tags_[hole] = tags_[next];
        hole = next;
      }
      next = (next + 1) & mask_;
    }
    slots_[hole] = 0;
    stamps_[e]++;
    free_.push_back(e);
    count_--;
  }

  void rebuild(size_t capacity) {
    std::vector<uint32_t> old_slots;
    old_slots.swap(slots_);
    slots_.assign(capacity, 0);
    tags_.assign(capacity, 0);
    mask_ = capacity - 1;
    for (uint32_t s : old_slots) {
      if (!s)
        continue;
      uint64_t h = mix(keys_[s - 1]);
      size_t slot = (size_t)h & mask_;
      while (slots_[slot])
        slot = (slot + 1) & mask_;
      slots_[slot] = s;
      tags_[slot] = tagOf(h);
    }
  }

  const HoldOps *ops_ = nullptr;
  void *client_ = nullptr;
  size_t size_ = 0;
  size_t mask_ = 0;
  size_t count_ = 0;
  int lowest_ = 0; // no live item in any bucket below this
  std::vector<uint32_t> slots_; // entry index + 1; 0 is empty
  std::vector<uint8_t> tags_;
  std::vector<uint64_t> keys_;
  std::vector<int> buckets_;
  std::vector<uint32_t> stamps_;
  std::vector<char> payloads_;
  std::vector<uint32_t> free_;
  std::vector<std::vector<Ref>> lists_;
  std::vector<long> live_;
  std::vector<char> retired_;
  unsigned long long absorbed_ = 0;
  unsigned long long inserted_ = 0;
};

#endif
