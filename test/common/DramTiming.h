#pragma once

#include <systemc.h>

#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>

#include "test/compiler/proto/voyager_ir.pb.h"

struct DramTimingConfig {
  bool present = false, timed = false;
  double bytes_per_cycle = 0, access_latency_cycles = 0, frequency_ghz = 0;

  static DramTimingConfig load(const voyager::MemoryConfig& fields) {
    DramTimingConfig config;
    config.present = fields.has_dram_mode();
    if (!config.present && !fields.has_dram_bytes_per_cycle() &&
        !fields.has_dram_access_latency_cycles()) return config;
    if (!config.present || !fields.has_dram_bytes_per_cycle() ||
        !fields.has_dram_access_latency_cycles() || !fields.has_frequency_ghz() ||
        !voyager::MemoryConfig_DramMode_IsValid(fields.dram_mode()))
      throw std::runtime_error("Incomplete DRAM timing in memory_config.txt");
    config.timed = fields.dram_mode() == voyager::MemoryConfig::TIMED;
    config.bytes_per_cycle = fields.dram_bytes_per_cycle();
    config.access_latency_cycles = fields.dram_access_latency_cycles();
    config.frequency_ghz = fields.frequency_ghz();
    if (!std::isfinite(config.frequency_ghz) || config.frequency_ghz <= 0 ||
        std::isnan(config.bytes_per_cycle) || config.bytes_per_cycle <= 0 ||
        !std::isfinite(config.access_latency_cycles) ||
        config.access_latency_cycles < 0 ||
        (!config.timed && (!std::isinf(config.bytes_per_cycle) ||
                          config.access_latency_cycles != 0)))
      throw std::runtime_error("Invalid DRAM timing in memory_config.txt");
    return config;
  }

  void validate_clock(sc_core::sc_time period) const {
    if (!present) return;
    const auto expected = sc_core::sc_time(1.0 / frequency_ghz, sc_core::SC_NS);
    if (!expected.value() || period != expected) {
      std::ostringstream message;
      message << "Harness clock period " << period
              << " disagrees with compiler frequency " << frequency_ghz
              << " GHz (expected " << expected << ") in memory_config.txt";
      throw std::runtime_error(message.str());
    }
  }

  double cycles(uint64_t bytes) const {
    if (!timed || !bytes) return 0;
    // The compiler uses fractional payload cycles. Round only the payload
    // to a bus cycle. Preserve fractional access latency from the compiler.
    return access_latency_cycles + std::ceil(bytes / bytes_per_cycle);
  }
};
