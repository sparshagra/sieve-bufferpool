#pragma once
#include <string>
#include <vector>

#include "policy.h"

// A workload: a name (used as the CSV column value and PNG filename stem), the
// number of distinct page ids it touches, and the request sequence itself.
struct Trace {
  std::string name;
  size_t key_space;
  std::vector<page_id_t> requests;
};

// Zipfian: page i is requested with probability proportional to 1/(i+1)^alpha.
// Higher alpha = more skew = a small "hot set" absorbs most requests.
Trace make_zipfian_trace(double alpha, size_t key_space, size_t length, unsigned seed);

// Every page 0..key_space-1 once, repeated -- LRU's worst case: nothing is ever
// re-requested before the cache has cycled all the way around.
Trace make_sequential_scan_trace(size_t key_space, size_t length);

// The trace is split into phases; each phase has its own "hot" sub-range that
// most requests land in, and the hot range shifts to a new sub-range each
// phase. Tests whether a policy can adapt when the working set moves, not
// just whether it can exploit one that stays put.
Trace make_hot_set_shift_trace(size_t key_space, size_t length, unsigned seed);
