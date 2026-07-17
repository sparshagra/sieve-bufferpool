#include "traces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

Trace make_zipfian_trace(double alpha, size_t key_space, size_t length, unsigned seed) {
  std::vector<double> weights(key_space);
  for (size_t i = 0; i < key_space; ++i) {
    weights[i] = 1.0 / std::pow(static_cast<double>(i + 1), alpha);
  }
  std::discrete_distribution<int> dist(weights.begin(), weights.end());
  std::mt19937 rng(seed);

  char name_buf[32];
  std::snprintf(name_buf, sizeof(name_buf), "zipf_a%.1f", alpha);

  Trace t;
  t.name = name_buf;
  t.key_space = key_space;
  t.requests.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    t.requests.push_back(static_cast<page_id_t>(dist(rng)));
  }
  return t;
}

Trace make_sequential_scan_trace(size_t key_space, size_t length) {
  Trace t;
  t.name = "sequential_scan";
  t.key_space = key_space;
  t.requests.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    t.requests.push_back(static_cast<page_id_t>(i % key_space));
  }
  return t;
}

Trace make_hot_set_shift_trace(size_t key_space, size_t length, unsigned seed) {
  constexpr int kNumPhases = 5;
  constexpr double kHotFraction = 0.15;   // hot set is 15% of the key space
  constexpr double kHotAccessProb = 0.9;  // 90% of requests land in the hot set

  size_t hot_size = std::max<size_t>(1, static_cast<size_t>(key_space * kHotFraction));
  size_t phase_len = std::max<size_t>(1, length / kNumPhases);
  size_t phase_stride = key_space / kNumPhases;

  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> coin(0.0, 1.0);

  Trace t;
  t.name = "hot_set_shift";
  t.key_space = key_space;
  t.requests.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    size_t phase = std::min<size_t>(kNumPhases - 1, i / phase_len);
    size_t hot_start = (phase * phase_stride) % key_space;
    page_id_t pid;
    if (coin(rng) < kHotAccessProb) {
      pid = static_cast<page_id_t>((hot_start + rng() % hot_size) % key_space);
    } else {
      pid = static_cast<page_id_t>(rng() % key_space);
    }
    t.requests.push_back(pid);
  }
  return t;
}
