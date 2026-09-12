// Randomized model check for CombiningHold, built without Charm++:
//
//   c++ -O2 -std=c++17 -I.. test_combine.cpp -o test_combine && ./test_combine
//
// Items are (key, value) with a min-combine and bucket = value / 16. The model
// is a std::map from key to the value the hold should be carrying. After every
// operation the invariants that the library relies on are checked:
//
//   * conservation: every item inserted is exactly one of held, released, or
//     retired through on_absorb -- the property SSSP's termination test needs;
//   * the hold carries the minimum for each key, in that minimum's bucket;
//   * release is lowest-bucket-first and never exceeds its bound;
//   * live(b) is exact.
#include "../htram_combine.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

struct Item {
  long key;
  long value;
};

static long retired_count = 0;
static uint64_t key_of(const void *p) { return (uint64_t)((const Item *)p)->key; }
static bool min_combine(void *held, const void *incoming, void *retired) {
  Item *h = (Item *)held;
  const Item *in = (const Item *)incoming;
  if (in->value < h->value) {
    std::memcpy(retired, h, sizeof(Item));
    *h = *in;
    return true;
  }
  std::memcpy(retired, in, sizeof(Item));
  return false;
}
static void on_absorb(void *, const void *) { retired_count++; }

static const int BUCKETS = 64;
static int bucket_of(long v) { return (int)(v / 16 < BUCKETS ? v / 16 : BUCKETS - 1); }

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

int main() {
  HoldOps ops{sizeof(Item), key_of, min_combine, on_absorb};
  for (int trial = 0; trial < 200; trial++) {
    std::mt19937_64 rng(trial);
    CombiningHold hold;
    hold.init(&ops, nullptr, BUCKETS);
    std::map<long, long> model;
    long inserted = 0, released = 0;
    retired_count = 0;
    long key_range = 1 + (long)(rng() % 5000);
    for (int step = 0; step < 20000; step++) {
      if (rng() % 8) {
        Item it{(long)(rng() % key_range), (long)(rng() % (16 * BUCKETS))};
        auto r = hold.insert(&it, bucket_of(it.value));
        inserted++;
        auto m = model.find(it.key);
        CHECK(r.absorbed == (m != model.end()));
        if (m == model.end())
          model[it.key] = it.value;
        else if (it.value < m->second)
          m->second = it.value;
        CHECK(r.new_bucket == bucket_of(model[it.key]));
      } else {
        int high = (int)(rng() % BUCKETS);
        long limit = 1 + (long)(rng() % 300);
        int last_bucket = -1;
        long got = hold.release(high, limit, [&](void *p) {
          Item *it = (Item *)p;
          auto m = model.find(it->key);
          CHECK(m != model.end());
          CHECK(m->second == it->value);
          int b = bucket_of(it->value);
          CHECK(b <= high);
          CHECK(b >= last_bucket);
          last_bucket = b;
          model.erase(m);
        });
        CHECK(got <= limit);
        released += got;
        // Anything left at or below `high` means release stopped at the limit.
        if (got < limit)
          for (auto &kv : model)
            CHECK(bucket_of(kv.second) > high);
      }
      CHECK(hold.size() == (long)model.size());
      CHECK(inserted == (long)model.size() + released + retired_count);
      if (step % 997 == 0) {
        long counts[BUCKETS] = {0};
        for (auto &kv : model)
          counts[bucket_of(kv.second)]++;
        for (int b = 0; b < BUCKETS; b++)
          CHECK(hold.live(b) == counts[b]);
      }
    }
    // Drain: everything left must come out exactly once.
    long rest = hold.release(BUCKETS - 1, 1L << 40, [&](void *p) {
      Item *it = (Item *)p;
      CHECK(model.erase(it->key) == 1);
    });
    released += rest;
    CHECK(model.empty());
    CHECK(hold.size() == 0);
    CHECK(inserted == released + retired_count);
    CHECK((long)hold.absorbed() == retired_count);
  }
  std::printf("test_combine: 200 trials passed\n");
  return 0;
}
