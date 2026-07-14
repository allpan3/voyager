// Native SystemC test for the CIM MatrixUnit direct-memory hierarchy

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "MatrixUnit.h"
#include "TypeToBits.h"

static constexpr int ROWS = IC_DIMENSION;
static constexpr int COLS = OC_DIMENSION;
static constexpr int CLOCK_NS = 10;

using Buffer = ACCUM_BUFFER_DATATYPE;
using BufferVector = Pack1D<Buffer, COLS>;

// Construct the lightweight access counter used by the native input buffer
AccessCounter::AccessCounter() = default;

// Count one named native input-buffer access
void AccessCounter::increment(const std::string &module_name) {
  access_counts[module_name]++;
}

// Count a width-weighted native input-buffer access
void AccessCounter::increment(const std::string &module_name, int count) {
  access_counts[module_name] += count;
}

// Identify the full-unit schedules used for reuse, reload, and accumulation
enum MatrixJob { MATRIX_REUSE, MATRIX_RELOAD, MATRIX_ACCUMULATION };

// Exercise serialized parameters and all native MatrixUnit memory boundaries
SC_MODULE(MatrixUnitTb) {
  MatrixUnit dut;
  sc_clock clk;
  sc_signal<bool> rstn;

  Connections::Combinational<ac_int<64, false>> serial_params_channel;
  Connections::Combinational<MemoryRequest> input_req_channel;
  Connections::Combinational<ac_int<IC_PORT_WIDTH, false>> input_resp_channel;
  Connections::Combinational<MemoryRequest> weight_req_channel;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> weight_resp_channel;
  Connections::Combinational<MemoryRequest> bias_req_channel;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> bias_resp_channel;
  Connections::Combinational<BufferVector> output_channel;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> output_data_channel;
  Connections::Combinational<ac_int<ADDRESS_WIDTH, false>> output_addr_channel;
  Connections::SyncChannel start_channel;
  Connections::SyncChannel done_channel;

  std::deque<uint64_t> expected_input_addresses;
  std::deque<uint64_t> expected_weight_addresses;
  std::deque<uint64_t> expected_bias_addresses;
  int input_request_count;
  int weight_request_count;
  int bias_request_count;
  bool test_failed;

  SC_HAS_PROCESS(MatrixUnitTb);

  // Construct the complete MatrixUnit and deterministic memory peers
  explicit MatrixUnitTb(sc_module_name name)
      : sc_module(name),
        dut("dut"),
        clk("clk", CLOCK_NS, SC_NS),
        start_channel("start_channel"),
        done_channel("done_channel"),
        input_request_count(0),
        weight_request_count(0),
        bias_request_count(0),
        test_failed(false) {
    dut.clk(clk);
    dut.rstn(rstn);
    dut.serial_params_in(serial_params_channel);
    dut.input_req(input_req_channel);
    dut.input_resp(input_resp_channel);
    dut.weight_req(weight_req_channel);
    dut.weight_resp(weight_resp_channel);
    dut.bias_req(bias_req_channel);
    dut.bias_resp(bias_resp_channel);
    dut.output_channel(output_channel);
    dut.output_data(output_data_channel);
    dut.output_addr(output_addr_channel);
    dut.start(start_channel);
    dut.done(done_channel);

    SC_THREAD(run);
    sensitive << clk.posedge_event();

    SC_THREAD(run_input_memory);
    sensitive << clk.posedge_event();

    SC_THREAD(run_weight_memory);
    sensitive << clk.posedge_event();

    SC_THREAD(run_bias_memory);
    sensitive << clk.posedge_event();

    SC_THREAD(watchdog);
  }

  // Advance one MatrixUnit cycle
  void tick() {
    wait(clk.posedge_event());
    wait(SC_ZERO_TIME);
  }

  // Record a deterministic failure while allowing channel peers to drain
  void require(bool condition, const std::string &message) {
    if (condition) {
      return;
    }
    std::cerr << "[FAIL] " << message << std::endl;
    test_failed = true;
  }

  // Stop a serialized-parameter, memory, or completion deadlock
  void watchdog() {
    wait(500, SC_US);
    require(false, "timed out waiting for MatrixUnit completion");
    sc_stop();
  }

  // Return the number of full-array operations in one job
  static int operation_count(MatrixJob job) {
    return job == MATRIX_ACCUMULATION ? 2 : 4;
  }

  // Return the number of completed output vectors in one job
  static int output_count(MatrixJob job) {
    return job == MATRIX_ACCUMULATION ? 1 : 4;
  }

  // Return the K2 extent in one job
  static int k2_extent(MatrixJob job) { return job == MATRIX_RELOAD ? 4 : 1; }

  // Return the C2 extent in one job
  static int c2_extent(MatrixJob job) {
    return job == MATRIX_ACCUMULATION ? 2 : 1;
  }

  // Assign a non-overlapping input memory range to one job
  static uint64_t input_offset(MatrixJob job) {
    return 0x1000ULL + 0x1200ULL * static_cast<int>(job);
  }

  // Assign a non-overlapping weight memory range to one job
  static uint64_t weight_offset(MatrixJob job) {
    return 0x5000ULL + 0x1200ULL * static_cast<int>(job);
  }

  // Assign the accumulation job bias memory range
  static uint64_t bias_offset(MatrixJob job) {
    return 0x9000ULL + 0x1200ULL * static_cast<int>(job);
  }

  // Build matching compute, input-fetch, and weight-address geometry
  MatrixParams make_params(MatrixJob job) const {
    MatrixParams params;
    for (int level = 0; level < 2; level++) {
      for (int loop = 0; loop < 6; loop++) {
        params.loops[level][loop] = 1;
      }
      for (int loop = 0; loop < 5; loop++) {
        params.weight_addr_loops[level][loop] = 1;
      }
    }

    params.x_loop_idx[0] = 1;
    params.y_loop_idx[0] = 0;
    params.weight_loop_idx[0] = 2;
    params.fy_loop_idx[0] = 3;
    params.reduction_loop_idx[0] = 4;
    params.fy_loop_idx[1] = 0;
    params.fx_loop_idx = 1;
    params.y_loop_idx[1] = 2;
    params.weight_loop_idx[1] = 3;
    params.x_loop_idx[1] = 4;
    params.reduction_loop_idx[1] = 5;
    params.weight_reuse_idx[0] = 0;
    params.weight_reuse_idx[1] = 1;

    if (job == MATRIX_REUSE) {
      params.weight_loop_idx[0] = 0;
      params.x_loop_idx[0] = 1;
      params.y_loop_idx[0] = 2;
      params.fy_loop_idx[0] = 3;
      params.reduction_loop_idx[0] = 4;
      params.loops[0][params.x_loop_idx[0]] = 4;
    } else {
      // Reload and accumulation live at L0 so each bank fill is one tile
      params.loops[0][params.weight_loop_idx[0]] = k2_extent(job);
      params.loops[0][params.reduction_loop_idx[0]] = c2_extent(job);
    }

    params.weight_addr_weight_loop_idx[0] = 0;
    params.weight_addr_reduction_loop_idx[0] = 1;
    params.weight_addr_fy_idx[0] = 2;
    // Mirror the mapper's shared weight-address layout: FY, FX, C1, C0, K1
    params.weight_addr_fy_idx[1] = 0;
    params.weight_addr_fx_idx = 1;
    params.weight_addr_reduction_loop_idx[1] = 2;
    params.weight_addr_reduction_loop_idx[2] = 3;
    params.weight_addr_weight_loop_idx[1] = 4;
    params.weight_addr_loops[0][params.weight_addr_weight_loop_idx[0]] =
        k2_extent(job);
    params.weight_addr_loops[0][params.weight_addr_reduction_loop_idx[0]] =
        c2_extent(job);
    params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[2]] =
        ROWS;

    params.input_offset = input_offset(job);
    params.weight_offset = weight_offset(job);
    params.bias_offset = bias_offset(job);
    params.input_dtype = 0;
    params.weight_dtype = 0;
    params.output_dtype = 0;
    params.input_burst_size = ROWS * INPUT_DTYPE_WIDTH / 8;
    params.input_num_beats = 1;
    params.input_pack_factor_lg2 = 0;
    params.weight_burst_size = COLS * WEIGHT_DTYPE_WIDTH / 8;
    params.weight_num_beats = 1;
    params.weight_pack_factor_lg2 = 0;
    params.input_y = 1;
    params.input_x = job == MATRIX_REUSE ? 4 : 1;
    params.stride = 1;
    params.padding = 0;
    params.has_bias = job == MATRIX_ACCUMULATION;
    params.output_to_memory = false;
    params.use_input_codebook = false;
    params.use_weight_codebook = false;
    params.weight_transpose = false;
    return params;
  }

  // Serialize one MatrixParams packet exactly as the accelerator harness does
  void send_params(MatrixParams params) {
    ac_int<MatrixParams::width, false> serialized_params;
    vector_to_type(TypeToBits<MatrixParams>(params), false, &serialized_params);
    constexpr int padded_width = ((MatrixParams::width + 63) / 64) * 64;
    ac_int<padded_width, false> padded_params = serialized_params;
    for (int word = 0; word < padded_width / 64; word++) {
      serial_params_channel.Push(padded_params.template slc<64>(word * 64));
    }
  }

  // Return the input request address used by one operation
  static uint64_t input_address(MatrixJob job, int operation) {
    if (job == MATRIX_RELOAD) {
      return input_offset(job);
    }
    return input_offset(job) + operation * ROWS * INPUT_DTYPE_WIDTH / 8;
  }

  // Return the K2 coordinate used by one operation
  static int operation_k2(MatrixJob job, int operation) {
    return job == MATRIX_RELOAD ? operation : 0;
  }

  // Return the C2 coordinate used by one operation
  static int operation_c2(MatrixJob job, int operation) {
    return job == MATRIX_ACCUMULATION ? operation : 0;
  }

  // Return the direct weight-row request address used by one operation
  static uint64_t weight_address(MatrixJob job, int operation, int row) {
    const int k2 = operation_k2(job, operation);
    const int c2 = operation_c2(job, operation);
    const uint64_t c_stride = k2_extent(job) * COLS * WEIGHT_DTYPE_WIDTH / 8;
    return weight_offset(job) + (c2 * ROWS + row) * c_stride +
           k2 * COLS * WEIGHT_DTYPE_WIDTH / 8;
  }

  // Generate one signed input element from its request address and row
  static int input_value(uint64_t address, int row) {
    return static_cast<int>((address + row) % 5) - 2;
  }

  // Generate one signed weight element from its request address and output lane
  static int weight_value(uint64_t address, int output) {
    return static_cast<int>((address + 3 * output) % 7) - 3;
  }

  // Generate one signed bias element from its request address and output lane
  static int bias_value(uint64_t address, int output) {
    return static_cast<int>((address / (COLS * Buffer::width / 8) + output) %
                            17) -
           8;
  }

  // Calculate one expected full-unit result vector element
  static int expected_value(MatrixJob job, int result, int output) {
    const int first_operation = job == MATRIX_ACCUMULATION ? 0 : result;
    const int contributions = job == MATRIX_ACCUMULATION ? 2 : 1;
    int expected =
        job == MATRIX_ACCUMULATION ? bias_value(bias_offset(job), output) : 0;
    for (int contribution = 0; contribution < contributions; contribution++) {
      const int operation = first_operation + contribution;
      const uint64_t input = input_address(job, operation);
      for (int row = 0; row < ROWS; row++) {
        expected += input_value(input, row) *
                    weight_value(weight_address(job, operation, row), output);
      }
    }
    return expected;
  }

  // Queue all expected memory requests before releasing reset
  void append_expected_requests(MatrixJob job) {
    // The reload job refetches its unchanged input once per L0 weight tile
    for (int operation = 0; operation < operation_count(job); operation++) {
      expected_input_addresses.push_back(input_address(job, operation));
    }

    for (int operation = 0; operation < operation_count(job); operation++) {
      if (job == MATRIX_REUSE && operation != 0) {
        continue;
      }
      for (int row = 0; row < ROWS; row++) {
        expected_weight_addresses.push_back(
            weight_address(job, operation, row));
      }
    }
    if (job == MATRIX_ACCUMULATION) {
      expected_bias_addresses.push_back(bias_offset(job));
    }
  }

  // Validate one request against its exact address and burst contract
  void validate_request(const MemoryRequest &request,
                        std::deque<uint64_t> &addresses, int burst_size,
                        const std::string &kind) {
    require(!addresses.empty(), "received unexpected " + kind + " request");
    if (!addresses.empty()) {
      const uint64_t expected = addresses.front();
      addresses.pop_front();
      std::ostringstream message;
      message << kind << " address expected 0x" << std::hex << expected
              << " got 0x" << request.address.to_uint64();
      require(request.address.to_uint64() == expected, message.str());
    }
    std::ostringstream message;
    message << kind << " burst expected " << burst_size << " got "
            << request.burst_size.to_uint();
    require(request.burst_size.to_uint() == static_cast<unsigned>(burst_size),
            message.str());
  }

  // Respond to native input-vector memory requests
  void run_input_memory() {
    input_req_channel.ResetRead();
    input_resp_channel.ResetWrite();
    wait();
    while (true) {
      const MemoryRequest request = input_req_channel.Pop();
      validate_request(request, expected_input_addresses,
                       ROWS * INPUT_DTYPE_WIDTH / 8, "input");
      input_request_count++;
      wait();

      ac_int<IC_PORT_WIDTH, false> response = 0;
      for (int row = 0; row < ROWS; row++) {
        const ac_int<INPUT_DTYPE_WIDTH, true> value =
            input_value(request.address.to_uint64(), row);
        response.set_slc(row * INPUT_DTYPE_WIDTH,
                         value.template slc<INPUT_DTYPE_WIDTH>(0));
      }
      input_resp_channel.Push(response);
    }
  }

  // Respond to direct native weight-row memory requests
  void run_weight_memory() {
    weight_req_channel.ResetRead();
    weight_resp_channel.ResetWrite();
    wait();
    while (true) {
      const MemoryRequest request = weight_req_channel.Pop();
      validate_request(request, expected_weight_addresses,
                       COLS * WEIGHT_DTYPE_WIDTH / 8, "weight");
      weight_request_count++;
      wait();
      wait();

      ac_int<OC_PORT_WIDTH, false> response = 0;
      for (int output = 0; output < COLS; output++) {
        const ac_int<WEIGHT_DTYPE_WIDTH, true> value =
            weight_value(request.address.to_uint64(), output);
        response.set_slc(output * WEIGHT_DTYPE_WIDTH,
                         value.template slc<WEIGHT_DTYPE_WIDTH>(0));
      }
      weight_resp_channel.Push(response);
    }
  }

  // Respond to direct bias memory requests over all OC-port beats
  void run_bias_memory() {
    bias_req_channel.ResetRead();
    bias_resp_channel.ResetWrite();
    wait();
    while (true) {
      const MemoryRequest request = bias_req_channel.Pop();
      validate_request(request, expected_bias_addresses,
                       COLS * Buffer::width / 8, "bias");
      bias_request_count++;
      wait();

      ac_int<Buffer::width * COLS, false> bits = 0;
      for (int output = 0; output < COLS; output++) {
        Buffer value;
        value.set_bits(bias_value(request.address.to_uint64(), output));
        bits.set_slc(output * Buffer::width, value.bits_rep());
      }
      constexpr int beats =
          (Buffer::width * COLS + OC_PORT_WIDTH - 1) / OC_PORT_WIDTH;
      for (int beat = 0; beat < beats; beat++) {
        bias_resp_channel.Push(
            bits.template slc<OC_PORT_WIDTH>(beat * OC_PORT_WIDTH));
      }
    }
  }

  // Run one serialized job, check start/done and outputs, and report
  // utilization
  double run_job(MatrixJob job) {
    send_params(make_params(job));
    start_channel.SyncPop();
    const sc_time start_time = sc_time_stamp();

    for (int result = 0; result < output_count(job); result++) {
      const BufferVector outputs = output_channel.Pop();
      for (int output = 0; output < COLS; output++) {
        std::ostringstream message;
        message << "job " << static_cast<int>(job) << " result " << result
                << " output " << output << " expected "
                << expected_value(job, result, output) << " got "
                << outputs[output].int_val.to_int();
        require(outputs[output].int_val.to_int() ==
                    expected_value(job, result, output),
                message.str());
      }
    }
    done_channel.SyncPop();

    const double measured_cycles =
        (sc_time_stamp() - start_time).to_seconds() / (CLOCK_NS * 1e-9);
    const int logical_macs = operation_count(job) * ROWS * COLS;
    const int peak_macs_per_cycle = ROWS * COLS;
    const double ideal_cycles =
        static_cast<double>(logical_macs) / peak_macs_per_cycle;
    const double utilization = ideal_cycles / measured_cycles;
    std::cout << std::fixed << std::setprecision(4)
              << "[UTIL] job=" << static_cast<int>(job)
              << " logical_macs=" << logical_macs
              << " peak_macs_per_cycle=" << peak_macs_per_cycle
              << " ideal_cycles=" << ideal_cycles
              << " measured_cycles=" << measured_cycles
              << " utilization=" << utilization << std::endl;
    require(measured_cycles >= ideal_cycles,
            "measured cycles must not be shorter than the full-array ideal");
    require(utilization > 0.0 && utilization <= 1.0,
            "utilization must be in the interval (0, 1]");
    return utilization;
  }

  // Run no-bias reuse, no-bias reload, and bias-plus-temporal-accumulation jobs
  void run() {
    serial_params_channel.ResetWrite();
    output_channel.ResetRead();
    output_data_channel.ResetRead();
    output_addr_channel.ResetRead();
    start_channel.ResetRead();
    done_channel.ResetRead();

    append_expected_requests(MATRIX_REUSE);
    append_expected_requests(MATRIX_RELOAD);
    append_expected_requests(MATRIX_ACCUMULATION);

    rstn.write(false);
    tick();
    tick();
    rstn.write(true);
    tick();

    const double reuse_utilization = run_job(MATRIX_REUSE);
    const double reload_utilization = run_job(MATRIX_RELOAD);
    run_job(MATRIX_ACCUMULATION);
    require(reuse_utilization > reload_utilization,
            "resident-weight reuse must outperform four direct reloads");

    tick();
    require(expected_input_addresses.empty(),
            "all expected input requests must be consumed");
    require(expected_weight_addresses.empty(),
            "all expected weight requests must be consumed");
    require(expected_bias_addresses.empty(),
            "all expected bias requests must be consumed");
    require(input_request_count == 10,
            "expected ten native input-vector requests");
    require(weight_request_count == ROWS * 7,
            "expected seven complete resident-set loads");
    require(bias_request_count == 1, "expected one direct bias request");

    if (!test_failed) {
      std::cout << "[PASS] matrix_unit_cim_direct_weights" << std::endl;
    }
    sc_stop();
  }
};

// Elaborate the full native CIM MatrixUnit test
int sc_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  MatrixUnitTb testbench("matrix_unit_cim_direct_weights");
  sc_start();
  return testbench.test_failed ? 1 : 0;
}
