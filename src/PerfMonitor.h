#pragma once

#include <ac_int.h>

#ifndef ENABLE_PERF_COUNTERS
#define ENABLE_PERF_COUNTERS 0
#endif

namespace MatrixPerformance {

using Counter = ac_int<32, false>;
using CounterIndex = ac_int<4, false>;
using SnapshotSequence = ac_int<16, false>;

// Stable indices for the optional synthesized matrix-performance counter bank
enum CounterId {
  SCHEMA_VERSION = 0,
  SNAPSHOT_SEQUENCE,
  CORE_CYCLES,
  ARRAY_RESIDENT_CYCLES,
  ARRAY_ISSUE_CYCLES,
  INPUT_UNAVAILABLE_CYCLES,
  INPUT_BACKPRESSURE_CYCLES,
  WEIGHT_UNAVAILABLE_CYCLES,
  WEIGHT_BACKPRESSURE_CYCLES,
  RESULT_BACKPRESSURE_CYCLES,
  ACCUMULATION_STALL_CYCLES,
  OUTPUT_BACKPRESSURE_CYCLES,
  OUTPUT_FIFO_FULL_CYCLES,
  COUNTER_COUNT
};

static constexpr int PERFORMANCE_COUNTER_COUNT = COUNTER_COUNT - CORE_CYCLES;
static constexpr int COMMON_PERFORMANCE_COUNTER_COUNT =
    OUTPUT_FIFO_FULL_CYCLES - CORE_CYCLES;
static constexpr unsigned SCHEMA_VERSION_VALUE = 2;

// Convert a public performance-counter ID to its compact storage index
static constexpr int storage_index(CounterId id) {
  return static_cast<int>(id) - static_cast<int>(CORE_CYCLES);
}

}  // namespace MatrixPerformance
