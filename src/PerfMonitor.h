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
  SNAPSHOT_SEQUENCE = 0,
  PROCESSOR_ACTIVE_CYCLES,
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
  MAC_WAIT_WEIGHT_SET_LOAD_CYCLES,
  CIM_COMPLETION_STORAGE_STALL_CYCLES,
  CIM_RESULT_SLOT_STALL_CYCLES,
  CIM_COMPLETION_DESCRIPTOR_STALL_CYCLES,
  COUNTER_COUNT
};

static constexpr int PERFORMANCE_COUNTER_COUNT =
    COUNTER_COUNT - PROCESSOR_ACTIVE_CYCLES;

// Convert a public performance-counter ID to its compact storage index
static constexpr int storage_index(CounterId id) {
  return static_cast<int>(id) - static_cast<int>(PROCESSOR_ACTIVE_CYCLES);
}

}  // namespace MatrixPerformance
