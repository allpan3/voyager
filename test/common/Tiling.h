#pragma once

#include <array>
#include <cstddef>
#include <iostream>

#include "test/common/Network.h"
#include "test/compiler/proto/param.pb.h"
#include "test/compiler/proto/tiling.pb.h"

constexpr std::size_t kSemanticLoopCount = 6;
constexpr std::size_t kSemanticLevelCount = 2;
constexpr std::size_t kSemanticL1Level = 0;
constexpr std::size_t kSemanticL2Level = 1;

// Canonical convolution dimensions independent of protobuf enum numbering
enum class SemanticLoop {
  OX = 0,
  OY,
  IC,
  OC,
  FX,
  FY,
};

// Complete inner-to-outer level with omitted dimensions appended at bound one
struct SemanticTilingLevel {
  std::array<int, kSemanticLoopCount> bounds;          // OX, OY, IC, OC, FX, FY
  std::array<SemanticLoop, kSemanticLoopCount> order;  // mapped then unit suffix
  std::size_t specified_loop_count;  // source-ordered prefix length

  // Return the explicit factor for one semantic dimension
  int bound(SemanticLoop loop) const {
    return bounds[static_cast<std::size_t>(loop)];
  }
};

// Named two-level schedule independent of compiler slots and row numbering
struct SemanticTiling {
  // Level 0 is L1 and level 1 is L2
  std::array<SemanticTilingLevel, kSemanticLevelCount> levels;
};

// Legacy compiler schedule with unit L2 FX and loops[0] at L2
struct Tiling {
  int loops[2][6];
  int x_loop_idx[2];
  int y_loop_idx[2];
  int reduction_loop_idx[2];
  int weight_loop_idx[2];
  int fx_loop_idx;  // L1 only
  int fy_loop_idx[2];
  int weight_reuse_idx[2];
  int stride;
  int padding;
  bool resnet_replication;
  bool generic_replication;
  int num_channels;
  int fx_unrolling;
  int input_x;
  int input_y;
};

std::ostream& operator<<(std::ostream& os, const Tiling& tiling);
SemanticTiling normalize_tiling(const voyager::Tiling& tiling);
Tiling get_interstellar_tiling(const voyager::Tiling& tiling);
Tiling lower_semantic_tiling(const SemanticTiling& tiling);
Tiling get_tiling(const Operation& operation);

Tiling get_conv2d_tiling(const codegen::OpOverload param);
Tiling get_linear_tiling(const codegen::OpOverload param);
Tiling get_pool2d_tiling(const codegen::OpOverload param);
void adjust_tiling_for_dimension(Tiling& tiling);
