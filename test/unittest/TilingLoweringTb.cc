#include <array>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "test/common/Tiling.h"

using ProtoLoopBound = std::pair<voyager::Loop, int>;

// Append one ordered mapping level
void append_level(voyager::Tiling& tiling,
                  std::initializer_list<ProtoLoopBound> bounds) {
  auto* level = tiling.add_level_tilings();
  for (const auto& bound : bounds) {
    auto* loop = level->add_loop_bounds();
    loop->set_loop(bound.first);
    loop->set_bound(bound.second);
  }
}

// Build one mapping ordered as L1 then L2
voyager::Tiling make_tiling(std::initializer_list<ProtoLoopBound> l1,
                            std::initializer_list<ProtoLoopBound> l2) {
  voyager::Tiling tiling;
  append_level(tiling, l1);
  append_level(tiling, l2);
  return tiling;
}

// Record one deterministic assertion failure
bool require(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

// Require one invalid mapping to fail with a useful diagnostic
bool expect_invalid_argument(const std::function<void()>& action,
                             const std::string& diagnostic) {
  try {
    action();
  } catch (const std::invalid_argument& error) {
    return require(
        std::string(error.what()).find(diagnostic) != std::string::npos,
        "expected diagnostic containing '" + diagnostic + "', got '" +
            error.what() + "'");
  }
  return require(false,
                 "expected invalid_argument containing '" + diagnostic + "'");
}

// Compare one normalized loop order
bool check_order(const SemanticTilingLevel& level,
                 const std::array<SemanticLoop, kSemanticLoopCount>& expected,
                 const std::string& label) {
  for (std::size_t i = 0; i < expected.size(); i++) {
    if (level.order[i] != expected[i]) {
      return require(
          false, label + " order mismatch at position " + std::to_string(i));
    }
  }
  return true;
}

// Compare one six-position loop row
bool check_loops(const int (&actual)[6], const std::array<int, 6>& expected,
                 const std::string& label) {
  for (std::size_t i = 0; i < expected.size(); i++) {
    if (actual[i] != expected[i]) {
      return require(
          false, label + " bound mismatch at position " + std::to_string(i));
    }
  }
  return true;
}

// Apply the controller's direct L1 semantic weight-reuse predicate
bool l1_output_reuses_weights(const Tiling& tiling, int output_loop_idx) {
  return (tiling.loops[1][tiling.weight_loop_idx[1]] == 1 ||
          tiling.weight_loop_idx[1] < output_loop_idx) &&
         (tiling.loops[1][tiling.reduction_loop_idx[1]] == 1 ||
          tiling.reduction_loop_idx[1] < output_loop_idx) &&
         (tiling.loops[1][tiling.fy_loop_idx[1]] == 1 ||
          tiling.fy_loop_idx[1] < output_loop_idx) &&
         (tiling.loops[1][tiling.fx_loop_idx] == 1 ||
          tiling.fx_loop_idx < output_loop_idx);
}

// Check explicit factors and total order at both semantic levels
bool test_semantic_normalization() {
  const auto normalized = normalize_tiling(
      make_tiling({{voyager::Loop::OY, 7}, {voyager::Loop::FX, 3}},
                  {{voyager::Loop::OX, 7},
                   {voyager::Loop::FY, 3},
                   {voyager::Loop::IC, 8},
                   {voyager::Loop::OC, 8}}));

  const auto& l1 = normalized.levels[kSemanticL1Level];
  const auto& l2 = normalized.levels[kSemanticL2Level];
  bool passed = true;
  passed &= require(l1.specified_loop_count == 2,
                    "l1 specified-loop count must be preserved");
  passed &= require(l2.specified_loop_count == 4,
                    "l2 specified-loop count must be preserved");
  passed &= require(l1.bounds == std::array<int, 6>{1, 7, 1, 1, 3, 1},
                    "l1 factors must explicitly contain OX,OY,IC,OC,FX,FY");
  passed &= require(l2.bounds == std::array<int, 6>{7, 1, 8, 8, 1, 3},
                    "l2 factors must explicitly contain OX,OY,IC,OC,FX,FY");
  passed &= check_order(l1,
                        {SemanticLoop::OY, SemanticLoop::FX, SemanticLoop::OX,
                         SemanticLoop::IC, SemanticLoop::OC, SemanticLoop::FY},
                        "l1");
  passed &= check_order(l2,
                        {SemanticLoop::OX, SemanticLoop::FY, SemanticLoop::IC,
                         SemanticLoop::OC, SemanticLoop::OY, SemanticLoop::FX},
                        "l2");
  return passed;
}

// Check structural, duplicate, enum, and bound validation
bool test_normalization_validation() {
  bool passed = true;

  voyager::Tiling one_level;
  append_level(one_level, {{voyager::Loop::OX, 2}});
  passed &= expect_invalid_argument([&] { normalize_tiling(one_level); },
                                    "exactly two temporal levels");

  auto three_levels = make_tiling({}, {});
  append_level(three_levels, {});
  passed &= expect_invalid_argument([&] { normalize_tiling(three_levels); },
                                    "exactly two temporal levels");

  const auto duplicate =
      make_tiling({{voyager::Loop::OX, 2}, {voyager::Loop::OX, 3}}, {});
  passed &= expect_invalid_argument([&] { normalize_tiling(duplicate); },
                                    "duplicate OX");

  const auto zero = make_tiling({{voyager::Loop::OY, 0}}, {});
  passed &= expect_invalid_argument([&] { normalize_tiling(zero); },
                                    "OY bound must be positive");

  const auto negative = make_tiling({}, {{voyager::Loop::IC, -2}});
  passed &= expect_invalid_argument([&] { normalize_tiling(negative); },
                                    "IC bound must be positive");

  const auto unsupported = make_tiling({{voyager::Loop::ON, 2}}, {});
  passed &= expect_invalid_argument([&] { normalize_tiling(unsupported); },
                                    "supports only OX");
  return passed;
}

// Preserve every loop position and factor in source order
bool test_complete_lowering() {
  const auto lowered =
      get_interstellar_tiling(make_tiling({{voyager::Loop::OY, 2},
                                           {voyager::Loop::FX, 3},
                                           {voyager::Loop::IC, 4},
                                           {voyager::Loop::OX, 5},
                                           {voyager::Loop::FY, 6},
                                           {voyager::Loop::OC, 7}},
                                          {{voyager::Loop::OX, 8},
                                           {voyager::Loop::FY, 9},
                                           {voyager::Loop::IC, 10},
                                           {voyager::Loop::FX, 1},
                                           {voyager::Loop::OC, 12},
                                           {voyager::Loop::OY, 13}}));

  bool passed = true;
  passed &= check_loops(lowered.loops[0], {13, 12, 10, 9, 8, 1}, "L2");
  passed &= check_loops(lowered.loops[1], {7, 6, 5, 4, 3, 2}, "L1");
  passed &= require(lowered.x_loop_idx[0] == 4 && lowered.y_loop_idx[0] == 0 &&
                        lowered.reduction_loop_idx[0] == 2 &&
                        lowered.weight_loop_idx[0] == 1 &&
                        lowered.fy_loop_idx[0] == 3,
                    "L2 positions must preserve order without fixed FX");
  passed &= require(lowered.x_loop_idx[1] == 2 && lowered.y_loop_idx[1] == 5 &&
                        lowered.reduction_loop_idx[1] == 3 &&
                        lowered.weight_loop_idx[1] == 0 &&
                        lowered.fx_loop_idx == 4 && lowered.fy_loop_idx[1] == 1,
                    "L1 positions must preserve the complete source order");
  return passed;
}

// Preserve omitted unit loops after the source-ordered prefix
bool test_partial_lowering() {
  const auto lowered = get_interstellar_tiling(
      make_tiling({{voyager::Loop::OY, 2}, {voyager::Loop::FX, 3}},
                  {{voyager::Loop::OX, 4}, {voyager::Loop::FY, 5}}));

  bool passed = true;
  passed &= check_loops(lowered.loops[0], {1, 1, 1, 5, 4, 1}, "partial L2");
  passed &= check_loops(lowered.loops[1], {1, 1, 1, 1, 3, 2}, "partial L1");
  passed &= require(lowered.x_loop_idx[0] == 4 && lowered.fy_loop_idx[0] == 3 &&
                        lowered.y_loop_idx[1] == 5 && lowered.fx_loop_idx == 4,
                    "omitted loop parameters must retain unique unit slots");
  return passed;
}

// Preserve reuse across a unit weight loop outside an output loop
bool test_unit_weight_loop_reuse() {
  const auto lowered =
      get_interstellar_tiling(make_tiling({{voyager::Loop::OY, 2},
                                           {voyager::Loop::OC, 1},
                                           {voyager::Loop::OX, 3},
                                           {voyager::Loop::IC, 4},
                                           {voyager::Loop::FX, 5},
                                           {voyager::Loop::FY, 6}},
                                          {}));

  bool passed = true;
  passed &=
      check_loops(lowered.loops[1], {6, 5, 4, 3, 1, 2}, "unit-weight reuse L1");
  passed &=
      require(lowered.x_loop_idx[1] == 3 && lowered.weight_loop_idx[1] == 4 &&
                  lowered.y_loop_idx[1] == 5,
              "semantic selectors must straddle the unit OC loop");
  passed &= require(l1_output_reuses_weights(lowered, lowered.x_loop_idx[1]),
                    "unit outer OC must not block OX weight reuse");
  passed &= require(l1_output_reuses_weights(lowered, lowered.y_loop_idx[1]),
                    "unit outer OC must not block OY weight reuse");
  return passed;
}

// Stop reuse at a live weight loop outside an output loop
bool test_live_weight_loop_reload() {
  const auto lowered =
      get_interstellar_tiling(make_tiling({{voyager::Loop::OY, 2},
                                           {voyager::Loop::OC, 2},
                                           {voyager::Loop::OX, 3},
                                           {voyager::Loop::IC, 4},
                                           {voyager::Loop::FX, 5},
                                           {voyager::Loop::FY, 6}},
                                          {}));

  bool passed = true;
  passed &= check_loops(lowered.loops[1], {6, 5, 4, 3, 2, 2},
                        "live-weight reload L1");
  passed &= require(!l1_output_reuses_weights(lowered, lowered.x_loop_idx[1]),
                    "live outer OC must force an OX weight reload");
  passed &= require(l1_output_reuses_weights(lowered, lowered.y_loop_idx[1]),
                    "OY inside the live OC loop must still reuse weights");
  return passed;
}

// Match the biased MobileBERT selector placement used by the exact SA gate
bool test_mobilebert_selector_reuse() {
  const auto lowered =
      get_interstellar_tiling(make_tiling({{voyager::Loop::OX, 128},
                                           {voyager::Loop::OC, 2},
                                           {voyager::Loop::OY, 1},
                                           {voyager::Loop::IC, 1},
                                           {voyager::Loop::FX, 1},
                                           {voyager::Loop::FY, 1}},
                                          {}));

  bool passed = true;
  passed &=
      check_loops(lowered.loops[1], {1, 1, 1, 1, 2, 128}, "MobileBERT L1");
  passed &= require(lowered.x_loop_idx[1] == 5 && lowered.y_loop_idx[1] == 3 &&
                        lowered.weight_loop_idx[1] == 4 &&
                        lowered.reduction_loop_idx[1] == 2 &&
                        lowered.fx_loop_idx == 1 && lowered.fy_loop_idx[1] == 0,
                    "MobileBERT semantic selectors must match its schedule");
  passed &= require(
      lowered.weight_reuse_idx[0] == 5 && lowered.weight_reuse_idx[1] == 5,
      "MobileBERT must serialize its single OX reuse selector");
  passed &= require(l1_output_reuses_weights(lowered, lowered.x_loop_idx[1]),
                    "MobileBERT OX must reuse the resident weight");
  passed &= require(!l1_output_reuses_weights(lowered, lowered.y_loop_idx[1]),
                    "MobileBERT OY must remain outside the live OC loop");
  return passed;
}

// Preserve OX and OY reuse across distinct convolution output selectors
bool test_distinct_conv_output_reuse() {
  const auto lowered =
      get_interstellar_tiling(make_tiling({{voyager::Loop::OX, 14},
                                           {voyager::Loop::OY, 28},
                                           {voyager::Loop::FX, 3},
                                           {voyager::Loop::FY, 3},
                                           {voyager::Loop::IC, 1},
                                           {voyager::Loop::OC, 1}},
                                          {}));

  bool passed = true;
  passed &= check_loops(lowered.loops[1], {1, 1, 3, 3, 28, 14},
                        "distinct-output convolution L1");
  passed &= require(lowered.x_loop_idx[1] == 5 && lowered.y_loop_idx[1] == 4 &&
                        lowered.weight_loop_idx[1] == 0 &&
                        lowered.reduction_loop_idx[1] == 1 &&
                        lowered.fy_loop_idx[1] == 2 && lowered.fx_loop_idx == 3,
                    "convolution OY and OX selectors must remain distinct");
  passed &= require(
      lowered.weight_reuse_idx[0] == 4 && lowered.weight_reuse_idx[1] == 5,
      "convolution must serialize both OY and OX reuse selectors");
  passed &= require(l1_output_reuses_weights(lowered, lowered.y_loop_idx[1]),
                    "convolution OY must reuse the resident weight");
  passed &= require(l1_output_reuses_weights(lowered, lowered.x_loop_idx[1]),
                    "convolution OX must reuse the resident weight");
  return passed;
}

// Reject outer FX until the legacy controller interface can represent it
bool test_outer_fx_rejected() {
  const auto tiling = make_tiling({}, {{voyager::Loop::FX, 2}});
  return expect_invalid_argument([&] { get_interstellar_tiling(tiling); },
                                 "L2 FX bound must be one");
}

// Run the focused normalization and lowering checks
int main() {
  const int passed =
      test_semantic_normalization() + test_normalization_validation() +
      test_complete_lowering() + test_partial_lowering() +
      test_unit_weight_loop_reuse() + test_live_weight_loop_reload() +
      test_mobilebert_selector_reuse() + test_distinct_conv_output_reuse() +
      test_outer_fx_rejected();
  std::cout << "Tiling lowering checks passed: " << passed << "/9" << std::endl;
  return passed == 9 ? 0 : 1;
}
