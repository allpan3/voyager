#include "test/common/Tiling.h"

#include <array>
#include <stdexcept>
#include <string>

#include "spdlog/spdlog.h"
#include "test/common/Utils.h"

namespace {

constexpr int kL2Level = 0;
constexpr int kL1Level = 1;

constexpr std::array<SemanticLoop, kSemanticLoopCount> kCanonicalLoopOrder = {
    SemanticLoop::OX, SemanticLoop::OY, SemanticLoop::IC,
    SemanticLoop::OC, SemanticLoop::FX, SemanticLoop::FY};

// Return a stable semantic loop name
const char* semantic_loop_name(SemanticLoop loop) {
  switch (loop) {
    case SemanticLoop::OX:
      return "OX";
    case SemanticLoop::OY:
      return "OY";
    case SemanticLoop::IC:
      return "IC";
    case SemanticLoop::OC:
      return "OC";
    case SemanticLoop::FX:
      return "FX";
    case SemanticLoop::FY:
      return "FY";
  }
  return "unknown";
}

// Translate one protobuf dimension into the semantic enum
SemanticLoop semantic_loop_from_proto(voyager::Loop loop) {
  switch (loop) {
    case voyager::Loop::OX:
      return SemanticLoop::OX;
    case voyager::Loop::OY:
      return SemanticLoop::OY;
    case voyager::Loop::IC:
      return SemanticLoop::IC;
    case voyager::Loop::OC:
      return SemanticLoop::OC;
    case voyager::Loop::FX:
      return SemanticLoop::FX;
    case voyager::Loop::FY:
      return SemanticLoop::FY;
    case voyager::Loop::ON:
      break;
    default:
      break;
  }
  throw std::invalid_argument(
      "tiling supports only OX, OY, IC, OC, FX, and FY");
}

// Return a readable temporal-level name
const char* semantic_level_name(std::size_t level) {
  return level == kSemanticL1Level ? "L1" : "L2";
}

// Enforce normalization invariants for directly constructed semantic schedules
void validate_semantic_level(const SemanticTilingLevel& level,
                             std::size_t level_index) {
  if (level.specified_loop_count > kSemanticLoopCount) {
    throw std::invalid_argument(
        "semantic tiling specified-loop count exceeds six");
  }

  std::array<bool, kSemanticLoopCount> seen{};
  for (SemanticLoop loop : level.order) {
    const auto index = static_cast<std::size_t>(loop);
    if (index >= kSemanticLoopCount || seen[index]) {
      throw std::invalid_argument("semantic tiling order is not a permutation");
    }
    seen[index] = true;
  }

  for (std::size_t i = 0; i < kSemanticLoopCount; i++) {
    if (level.bounds[i] <= 0) {
      throw std::invalid_argument(
          std::string(semantic_level_name(level_index)) +
          " semantic tiling bound must be positive");
    }
  }

  for (std::size_t i = level.specified_loop_count; i < kSemanticLoopCount;
       i++) {
    if (level.bound(level.order[i]) != 1) {
      throw std::invalid_argument(
          std::string(semantic_level_name(level_index)) +
          " source-omitted loop factors must be one");
    }
  }
}

// Initialize a compiler schedule with unit loop bounds
Tiling initialize_tiling() {
  Tiling tiling{};
  tiling.fx_loop_idx = -1;
  for (int level = 0; level < 2; level++) {
    for (int loop = 0; loop < 6; loop++) {
      tiling.loops[level][loop] = 1;
    }
    tiling.x_loop_idx[level] = -1;
    tiling.y_loop_idx[level] = -1;
    tiling.reduction_loop_idx[level] = -1;
    tiling.weight_loop_idx[level] = -1;
    tiling.fy_loop_idx[level] = -1;
  }
  tiling.weight_reuse_idx[0] = 0;
  tiling.weight_reuse_idx[1] = 0;
  tiling.stride = 1;
  tiling.resnet_replication = false;
  tiling.generic_replication = false;
  tiling.fx_unrolling = 1;
  return tiling;
}

// Assign one semantic dimension to one compiler loop position
void assign_loop(Tiling& tiling, std::size_t semantic_level,
                 const SemanticTilingLevel& level, SemanticLoop loop,
                 int slot) {
  const int storage_level = semantic_level == kSemanticL1Level ? 1 : 0;
  tiling.loops[storage_level][slot] = level.bound(loop);
  switch (loop) {
    case SemanticLoop::OX:
      tiling.x_loop_idx[storage_level] = slot;
      return;
    case SemanticLoop::OY:
      tiling.y_loop_idx[storage_level] = slot;
      return;
    case SemanticLoop::IC:
      tiling.reduction_loop_idx[storage_level] = slot;
      return;
    case SemanticLoop::OC:
      tiling.weight_loop_idx[storage_level] = slot;
      return;
    case SemanticLoop::FX:
      if (semantic_level == kSemanticL1Level) {
        tiling.fx_loop_idx = slot;
      }
      return;
    case SemanticLoop::FY:
      tiling.fy_loop_idx[storage_level] = slot;
      return;
  }
  throw std::logic_error("unreachable semantic loop");
}

// Preserve the legacy pair of innermost L1 spatial reuse slots
void set_legacy_weight_reuse_indices(Tiling& tiling) {
  if (tiling.x_loop_idx[kL1Level] == 5 ||
      tiling.y_loop_idx[kL1Level] == 5) {
    tiling.weight_reuse_idx[0] = 5;
    tiling.weight_reuse_idx[1] = 5;
  }
  if (tiling.x_loop_idx[kL1Level] == 4 ||
      tiling.y_loop_idx[kL1Level] == 4) {
    tiling.weight_reuse_idx[0] = 4;
  }
}

// Pack L1 fully and omit fixed-unit L2 FX from the five-slot legacy schedule
Tiling lower_tiling(const SemanticTiling& semantic_tiling) {
  Tiling tiling = initialize_tiling();
  for (std::size_t level_index = 0; level_index < kSemanticLevelCount;
       level_index++) {
    const auto& level = semantic_tiling.levels[level_index];
    int slot = level_index == kSemanticL1Level ? 5 : 4;
    for (std::size_t order_index = 0; order_index < kSemanticLoopCount;
         order_index++) {
      const SemanticLoop loop = level.order[order_index];
      if (level_index == kSemanticL2Level && loop == SemanticLoop::FX) {
        continue;
      }
      assign_loop(tiling, level_index, level, loop, slot--);
    }
    if (slot != -1) throw std::logic_error("legacy tiling row is incomplete");
  }

  set_legacy_weight_reuse_indices(tiling);
  return tiling;
}

// Return the semantic dimension assigned to one compiler-schedule slot
const char* tiling_loop_name(const Tiling& tiling, int level, int slot) {
  if (tiling.x_loop_idx[level] == slot) return "OX";
  if (tiling.y_loop_idx[level] == slot) return "OY";
  if (tiling.reduction_loop_idx[level] == slot) return "IC";
  if (tiling.weight_loop_idx[level] == slot) return "OC";
  if ((level == kL1Level && tiling.fx_loop_idx == slot) ||
      (level == kL2Level && slot == 5)) return "FX";
  if (tiling.fy_loop_idx[level] == slot) return "FY";
  return "?";
}

}  // namespace

// Preserve each source prefix and append every omitted dimension at unit bound
SemanticTiling normalize_tiling(const voyager::Tiling& tiling) {
  if (tiling.level_tilings_size() != static_cast<int>(kSemanticLevelCount)) {
    throw std::invalid_argument(
        "tiling must contain exactly two temporal levels");
  }

  SemanticTiling normalized{};
  for (std::size_t level_index = 0; level_index < kSemanticLevelCount;
       level_index++) {
    const auto& source = tiling.level_tilings(static_cast<int>(level_index));
    if (source.loop_bounds_size() > static_cast<int>(kSemanticLoopCount)) {
      throw std::invalid_argument(
          std::string(semantic_level_name(level_index)) +
          " tiling contains more than six loops");
    }

    auto& level = normalized.levels[level_index];
    level.bounds.fill(1);
    level.specified_loop_count =
        static_cast<std::size_t>(source.loop_bounds_size());
    std::array<bool, kSemanticLoopCount> seen{};

    for (int i = 0; i < source.loop_bounds_size(); i++) {
      const auto& source_loop = source.loop_bounds(i);
      const SemanticLoop loop = semantic_loop_from_proto(source_loop.loop());
      const auto loop_index = static_cast<std::size_t>(loop);
      if (seen[loop_index]) {
        throw std::invalid_argument(
            std::string(semantic_level_name(level_index)) +
            " tiling contains duplicate " + semantic_loop_name(loop));
      }
      if (source_loop.bound() <= 0) {
        throw std::invalid_argument(
            std::string(semantic_level_name(level_index)) + " " +
            semantic_loop_name(loop) + " bound must be positive");
      }

      seen[loop_index] = true;
      level.bounds[loop_index] = source_loop.bound();
      level.order[static_cast<std::size_t>(i)] = loop;
    }

    std::size_t order_index = level.specified_loop_count;
    for (SemanticLoop loop : kCanonicalLoopOrder) {
      if (!seen[static_cast<std::size_t>(loop)]) {
        level.order[order_index++] = loop;
      }
    }
    if (order_index != kSemanticLoopCount) {
      throw std::logic_error("normalized semantic order is incomplete");
    }
  }
  return normalized;
}

// Reject unsupported outer FX before packing the legacy compiler schedule
Tiling lower_semantic_tiling(const SemanticTiling& semantic_tiling) {
  validate_semantic_level(semantic_tiling.levels[kSemanticL1Level],
                          kSemanticL1Level);
  validate_semantic_level(semantic_tiling.levels[kSemanticL2Level],
                          kSemanticL2Level);

  if (semantic_tiling.levels[kSemanticL2Level].bound(SemanticLoop::FX) != 1) {
    throw std::invalid_argument("L2 FX bound must be one");
  }

  return lower_tiling(semantic_tiling);
}

std::ostream& operator<<(std::ostream& os, const Tiling& tiling) {
  os << "Schedule:" << std::endl;
  for (int level = 0; level < 2; level++) {
    os << (level == kL2Level ? "  L2 outer -> inner: "
                            : "  L1 outer -> inner: ");
    for (int slot = 0; slot < 6; slot++) {
      if (slot != 0) os << ", ";
      os << tiling_loop_name(tiling, level, slot) << "="
         << tiling.loops[level][slot];
    }
    os << std::endl;
  }
  os << "Weight Reuse Index: " << tiling.weight_reuse_idx[0] << " "
     << tiling.weight_reuse_idx[1] << std::endl;
  os << "Stride: " << tiling.stride << std::endl;
  os << "Padding: " << tiling.padding << std::endl;
  os << "Resnet Replication: " << tiling.resnet_replication << std::endl;
  os << "Generic Replication: " << tiling.generic_replication << std::endl;
  os << "Num Channels: " << tiling.num_channels << std::endl;
  os << "FX Unrolling: " << tiling.fx_unrolling << std::endl;
  os << "Padded Input X: " << tiling.input_x << std ::endl;
  os << "Padded Input Y: " << tiling.input_y << std::endl;
  return os;
}

Tiling get_tiling(const Operation& operation) {
  const auto param = operation.param;
  const auto op_list = get_op_list(param);
  const auto first_op = op_list[0];

  // get environment variable
  const char* env_var = std::getenv("MANUAL_TILING");
  bool manual_tiling = env_var ? std::stoi(env_var) : false;

  Tiling tiling;
  if (manual_tiling || !operation.has_valid_tiling) {
    spdlog::info("Using manual tiling for operation {} with target {}\n",
                 operation.name, first_op.target());
    if (first_op.target() == "conv2d" || first_op.target() == "conv2d_mx") {
      tiling = get_conv2d_tiling(first_op);
    } else {
      tiling = get_linear_tiling(first_op);
    }
  } else {
    tiling = get_interstellar_tiling(operation.tiling);
    if (first_op.kwargs().contains("stride")) {
      auto stride = first_op.kwargs().at("stride").int_list().values();
      tiling.stride = stride[0];
    } else {
      tiling.stride = 1;
    }

    if (first_op.kwargs().contains("padding")) {
      auto padding = first_op.kwargs().at("padding").int_list().values();
      tiling.padding = padding[0];
    } else {
      tiling.padding = 0;
    }
  }

  const auto input = first_op.kwargs().at("input").tensor();
  const auto input_shape = get_shape(input);

  if (first_op.target() == "conv2d" || first_op.target() == "conv2d_mx") {
    tiling.input_y = input_shape[1];
    tiling.input_x = input_shape[2];
  } else {
    tiling.input_y = 1;
    tiling.input_x = get_size(input_shape) / input_shape.back();
  }

  return tiling;
}

// Complete omitted mapper dimensions before compiler-slot packing
Tiling get_interstellar_tiling(const voyager::Tiling& tiling) {
  return lower_semantic_tiling(normalize_tiling(tiling));
}

Tiling get_conv2d_tiling(const codegen::OpOverload param) {
  const auto kwargs = param.kwargs();

  const auto input = kwargs.at("input").tensor();
  const auto weight = kwargs.at("weight").tensor();
  const auto paddings = kwargs.at("padding").int_list().values();
  const auto dilation = kwargs.at("dilation").int_list().values();
  const auto strides = kwargs.at("stride").int_list().values();

  const auto input_shape = get_shape(input);
  const auto weight_shape = get_shape(weight);

  const int output_height = (input_shape[1] + 2 * paddings[0] -
                             dilation[0] * (weight_shape[0] - 1) - 1) /
                                strides[0] +
                            1;
  const int output_width = (input_shape[2] + 2 * paddings[1] -
                            dilation[1] * (weight_shape[1] - 1) - 1) /
                               strides[1] +
                           1;

  std::vector<int> output_shape = {
      output_height,
      output_width,
      input_shape[3],
      weight_shape[3],
  };

  int x1 = 1, y1 = 1, k1 = 1;
  int x0 = output_shape[2];
  int y0 = output_shape[1];
  int k0 = weight_shape[3] / OC_DIMENSION;
  int c0 = weight_shape[2] / IC_DIMENSION;
  int fx = weight_shape[1];
  int fy = weight_shape[0];
  int stride = strides[0];
  int padding = paddings[0];

  // conv2d (vit)
  if (input_shape[3] == 3 && weight_shape[0] == 16 && weight_shape[1] == 16) {
    int fx_unrolling;
    if (IC_DIMENSION == 4) {
      fx_unrolling = 1;
    } else if (IC_DIMENSION == 8) {
      fx_unrolling = 2;
    } else if (IC_DIMENSION == 16) {
      fx_unrolling = 4;
    } else if (IC_DIMENSION == 32 || IC_DIMENSION == 64) {
      fx_unrolling = 8;
    } else {
      throw std::runtime_error("replication not supported for IC_DIMENSION=" +
                               std::to_string(IC_DIMENSION));
    }

    int k1 = weight_shape[3] / 32;
    Tiling tiling = {
        .loops = {{7, 1, k1, 1, fy, 1}, {1, 2, 1, fx / fx_unrolling, 2, 14}},
        .x_loop_idx = {1, 5},
        .y_loop_idx = {0, 4},
        .reduction_loop_idx = {3, 0},
        .weight_loop_idx = {2, 1},
        .fx_loop_idx = 3,
        .fy_loop_idx = {4, 2},
        .weight_reuse_idx = {4, 5},
        .stride = stride,
        .padding = 0,
        .resnet_replication = false,
        .generic_replication = true,
        .num_channels = 3,
        .fx_unrolling = fx_unrolling,
    };

    if (IC_DIMENSION < 16) {
      tiling.loops[1][5] /= 2;
      tiling.loops[0][0] *= 2;
    }

    if (OC_DIMENSION < 16) {
      tiling.loops[0][tiling.weight_loop_idx[0]] *= (16 / OC_DIMENSION);
    } else if (OC_DIMENSION > 16) {
      int div_factor = OC_DIMENSION / 16;
      int& k1 = tiling.loops[0][tiling.weight_loop_idx[0]];
      while (k1 > 1 && k1 % 2 == 0 && div_factor > 1) {
        k1 /= 2;
        div_factor /= 2;
      }
      int& k0 = tiling.loops[1][tiling.weight_loop_idx[1]];
      while (k0 > 1 && k0 % 2 == 0 && div_factor > 1) {
        k0 /= 2;
        div_factor /= 2;
      }

      if (div_factor > 1) {
        spdlog::error("OC_DIMENSION is not a multiple of 16\n");
        exit(1);
      }
    }

    return tiling;
  }
  // conv1
  else if (input_shape[3] == 3 && weight_shape[0] == 7 &&
           weight_shape[1] == 7) {
    int fx;
    if (IC_DIMENSION == 4) {
      fx = 7;
    } else if (IC_DIMENSION == 8) {
      fx = 4;
    } else if (IC_DIMENSION == 16) {
      fx = 2;
    } else if (IC_DIMENSION == 32 || IC_DIMENSION == 64) {
      fx = 1;
    } else {
      throw std::runtime_error("replication not supported for IC_DIMENSION=" +
                               std::to_string(IC_DIMENSION));
    }

    int K1 = weight_shape[3] / 32;
    int Y1 = output_shape[0] / 16;
    int X1 = output_shape[1] / 16;
    Tiling tiling = {
        .loops = {{X1, Y1, K1, 1, 1, 1}, {1, 2, 7, fx, 16, 16}},
        .x_loop_idx = {0, 5},
        .y_loop_idx = {1, 4},
        .reduction_loop_idx = {3, 0},
        .weight_loop_idx = {2, 1},
        .fx_loop_idx = 3,
        .fy_loop_idx = {4, 2},
        .weight_reuse_idx = {4, 5},
        .stride = stride,
        .padding = 3,
        .resnet_replication = true,
        .generic_replication = false,
        .num_channels = 3,
    };

    if (IC_DIMENSION < 16) {
      tiling.loops[1][5] /= 2;
      tiling.loops[0][0] *= 2;
    }

    if (OC_DIMENSION < 16) {
      tiling.loops[0][tiling.weight_loop_idx[0]] *= (16 / OC_DIMENSION);
    } else if (OC_DIMENSION > 16) {
      int div_factor = OC_DIMENSION / 16;
      int& k1 = tiling.loops[0][tiling.weight_loop_idx[0]];
      while (k1 > 1 && k1 % 2 == 0 && div_factor > 1) {
        k1 /= 2;
        div_factor /= 2;
      }
      int& k0 = tiling.loops[1][tiling.weight_loop_idx[1]];
      while (k0 > 1 && k0 % 2 == 0 && div_factor > 1) {
        k0 /= 2;
        div_factor /= 2;
      }

      if (div_factor > 1) {
        spdlog::error("OC_DIMENSION is not a multiple of 16\n");
        exit(1);
      }
    }

    return tiling;
  }

  // Reduce OC0 to meet weight buffer constraint
  while (fx * fy * k0 * IC_DIMENSION > WEIGHT_BUFFER_SIZE) {
    if (k0 % 2 == 0) {
      k0 /= 2;
      k1 *= 2;
    } else {
      spdlog::error("Weight buffer is too small\n");
      exit(1);
    }
  }

  // Reduce OC0 to meet weight buffer constraint
  while (fx * fy * k0 * IC_DIMENSION > WEIGHT_BUFFER_SIZE) {
    if (k0 % 2 == 0) {
      k0 /= 2;
      k1 *= 2;
    } else {
      spdlog::error("Weight buffer is too small\n");
      exit(1);
    }
  }

  // Reduce X0 and Y0 to meet input buffer constraint. We are not counting
  // stride here because of the hardware implementation
  while (true) {
    int ix = x0 * stride + fx - 1;
    int iy = y0 * stride + fy - 1;
    if (ix * iy <= INPUT_BUFFER_SIZE) {
      break;
    }
    if (x0 % 2 == 0 && y0 % 2 == 0) {
      x0 /= 2;
      x1 *= 2;
      y0 /= 2;
      y1 *= 2;
    } else {
      spdlog::error("Input buffer is too small\n");
      exit(1);
    }
  }

  // Reduce either OC0, or OX0 and OY0, to meet accumulation buffer
  // constraint
  const int max_k0 = k0;
  while (x0 * y0 * k0 > ACCUM_BUFFER_SIZE) {
    if (k0 % 2 == 0) {
      k0 /= 2;
      k1 *= 2;
    } else if (x0 % 2 == 0 && y0 % 2 == 0) {
      x0 /= 2;
      x1 *= 2;
      y0 /= 2;
      y1 *= 2;
      // Since we are reducing both x0 and y0, there is a chance we can
      // increase k0
      if (k0 * 2 <= max_k0) {
        k0 *= 2;
        k1 /= 2;
      }
    } else {
      spdlog::error("Accumulation buffer is too small\n");
      exit(1);
    }
  }

  return {
      .loops = {{x1, y1, k1, c0, 1, 1}, {1, k0, fy, fx, y0, x0}},
      .x_loop_idx = {0, 5},
      .y_loop_idx = {1, 4},
      .reduction_loop_idx = {3, 0},
      .weight_loop_idx = {2, 1},
      .fx_loop_idx = 3,
      .fy_loop_idx = {4, 2},
      .weight_reuse_idx = {4, 5},
      .stride = stride,
      .padding = padding,
  };
}

Tiling get_linear_tiling(const codegen::OpOverload op) {
  const auto kwargs = op.kwargs();
  const auto input_shape = get_shape(kwargs.at("input").tensor());

  bool is_matmul = op.target().find("matmul") != std::string::npos;
  std::string weight_key = is_matmul ? "other" : "weight";
  const auto weight_shape = get_shape(kwargs.at(weight_key).tensor());

  int x1 = 1, k1 = 1, c1 = 1;
  int x0 = get_size(input_shape) / input_shape.back();
  int k0 = weight_shape[0] / OC_DIMENSION;
  int c0 = weight_shape[1] / IC_DIMENSION;

  // Manual tiling
  if (x0 == 128 && weight_shape[0] == 512 && weight_shape[1] == 2048) {
    return {
        .loops = {{1, k0 / 2, 4, c0 / 2, 1, 1}, {2, 1, 2, 1, 1, 32}},
        .x_loop_idx = {2, 5},
        .y_loop_idx = {0, 1},
        .reduction_loop_idx = {3, 0},
        .weight_loop_idx = {1, 2},
        .fx_loop_idx = 4,
        .fy_loop_idx = {4, 3},
        .weight_reuse_idx = {5, 5},
        .stride = 1,
        .resnet_replication = false,
    };
  }

  // torch.matmul weight is also an activation, thus does not need to be
  // transposed
  if (op.target() == "matmul" || op.target() == "matmul_mx") {
    int size = weight_shape.size();
    c0 = weight_shape[size - 2] / IC_DIMENSION;
    k0 = weight_shape[size - 1] / OC_DIMENSION;
  }

  // Loop indices cannot exceed 1024 (10-bit)
  while (x0 >= 1024 || x0 * c0 > INPUT_BUFFER_SIZE) {
    if (x0 % 2 == 0) {
      x0 /= 2;
      x1 *= 2;
    } else if (c0 % 2 == 0) {
      c0 /= 2;
      c1 *= 2;
    } else {
      spdlog::error("Input buffer is too small\n");
      exit(1);
    }
  }

  while (k0 * c0 * IC_DIMENSION > WEIGHT_BUFFER_SIZE) {
    if (k0 % 2 == 0) {
      k0 /= 2;
      k1 *= 2;
    } else if (c0 % 2 == 0) {
      c0 /= 2;
      c1 *= 2;
    } else {
      spdlog::error("Weight buffer is too small\n");
      exit(1);
    }
  }

  while (x0 * k0 > ACCUM_BUFFER_SIZE) {
    if (k0 % 2 == 0) {
      k0 /= 2;
      k1 *= 2;
    } else if (x0 % 2 == 0) {
      x0 /= 2;
      x1 *= 2;
    } else {
      spdlog::error("Accumulation buffer is too small\n");
      exit(1);
    }
  }

  return {
      .loops = {{x1, 1, k1, c1, 1, 1}, {c0, k0, 1, 1, 1, x0}},
      .x_loop_idx = {0, 5},
      .y_loop_idx = {1, 4},
      .reduction_loop_idx = {3, 0},
      .weight_loop_idx = {2, 1},
      .fx_loop_idx = 3,
      .fy_loop_idx = {4, 2},
      .weight_reuse_idx = {4, 5},
      .stride = 1,
      .padding = 0,
      .resnet_replication = false,
  };
}

Tiling get_pool2d_tiling(const codegen::OpOverload op) {
  const auto kwargs = op.kwargs();
  const auto input_shape = get_shape(kwargs.at("input").tensor());

  int Y = input_shape[1];
  int X = input_shape[2];
  int K = input_shape[3];

  int x0, y0, stride, padding, x1, y1, k0, actual_padding;

  Tiling tiling;

  if (kwargs.contains("output_size")) {
    const auto output_size = kwargs.at("output_size").int_list().values();
    int output_h = output_size[0];
    int output_w = output_size[1];

    stride = X / output_h;
    x0 = X - (output_h - 1) * stride;
    y0 = Y - (output_w - 1) * stride;

    x1 = X / x0;
    y1 = Y / y0;
    k0 = K / ACCUMULATOR_WIDTH;
    actual_padding = 0;
  } else {
    const auto kernel_size = kwargs.at("kernel_size").int_list().values();
    const auto strides = kwargs.at("stride").int_list().values();
    const auto paddings = kwargs.at("padding").int_list().values();

    y0 = kernel_size[0];
    x0 = kernel_size[1];
    stride = strides[0];
    padding = paddings[0];

    // calculate ouptut dimension (ignoring padding, which will be handled in
    // hw)
    x1 = (X + 2 * padding - x0) / stride + 1;
    y1 = (Y + 2 * padding - y0) / stride + 1;
    // pytorch assumes padding on all direction, not all of the values are used
    actual_padding = (x1 - 1) * stride + x0 - X;
    k0 = K / ACCUMULATOR_WIDTH;
  }

  return {
      .loops = {{x1, y1, 1, 1, 1, 1}, {1, k0, 1, 1, y0, x0}},
      .x_loop_idx = {0, 5},
      .y_loop_idx = {1, 4},
      .reduction_loop_idx = {3, 0},
      .weight_loop_idx = {2, 1},
      .fx_loop_idx = 3,
      .fy_loop_idx = {4, 2},
      .weight_reuse_idx = {4, 5},
      .stride = stride,
      .padding = actual_padding,
      .resnet_replication = false,
  };
}
