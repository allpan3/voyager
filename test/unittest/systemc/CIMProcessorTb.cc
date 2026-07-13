// SystemC test for the strict INT8 CIMProcessor matrix-backend contract

#ifdef SCVERIFY
#include <mc_scverify.h>
#define CIMPROCESSOR_DUT_TYPE(T) CCS_DESIGN(T)
#else
#define CIMPROCESSOR_DUT_TYPE(T) T
#endif

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <iostream>
#include <sstream>

#include "CIMProcessor.h"

static constexpr int CH_IN = 2;
static constexpr int CH_OUT = 2;
static constexpr int B_SETS = 2;
static constexpr int BASE_A_WIDTH = 4;
static constexpr int BASE_B_WIDTH = 4;
static constexpr int BASE_C_WIDTH = 12;
static constexpr int WRITE_CH_IN = 1;
static constexpr int MAC_LATENCY = 1;
static constexpr int MODE = 0;
static constexpr int A_WIDTH = 8;
static constexpr int B_WIDTH = 8;
static constexpr bool SIGNED = true;
static constexpr int TILE_INPUT_LANES = 2;
static constexpr int TILE_OUTPUT_LANES = 1;
static constexpr int REDUCTION_GROUPS = 2;
static constexpr int MULTICAST_GROUPS = 3;
static constexpr int ROWS = CH_IN * TILE_INPUT_LANES * REDUCTION_GROUPS;
static constexpr int ELEMENT_B_COLS = CH_OUT / (B_WIDTH / BASE_B_WIDTH);
static constexpr int COLS = ELEMENT_B_COLS * TILE_OUTPUT_LANES * MULTICAST_GROUPS;

using Processor =
    CIMProcessor<std::tuple<DataTypes::int8>, std::tuple<DataTypes::int8>, DataTypes::int8, DataTypes::int8,
                 DataTypes::int24, DataTypes::int24, DataTypes::fp8_e8m0, ROWS, COLS, 16, CH_IN, CH_OUT, B_SETS,
                 BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, SIGNED,
                 TILE_INPUT_LANES, TILE_OUTPUT_LANES, REDUCTION_GROUPS, MULTICAST_GROUPS, REDUCTION_GROUPS,
                 MULTICAST_GROUPS, MULTICAST_GROUPS, CIM_C_PORT_MULTICAST_MAJOR>;

using Dut = CIMPROCESSOR_DUT_TYPE(Processor);
using Buffer = DataTypes::int24;
using BufferVector = Pack1D<Buffer, COLS>;

// CIMProcessorTb checks full-vector spatial reduction and buffer-backed
// accumulation
SC_MODULE(CIMProcessorTb) {
  Dut dut;
  sc_clock clk;
  sc_signal<bool> rstn;

  Connections::Combinational<ac_int<INPUT_BUFFER_WIDTH, false>> input_channel;
  Connections::Combinational<ac_int<WEIGHT_BUFFER_WIDTH, false>> weight_channel;
  Connections::Combinational<BufferVector> bias_channel;
  Connections::Combinational<MatrixParams> params_channel;
  Connections::Combinational<BufferVector> output_channel;
  Connections::SyncChannel start_channel;

  Connections::Combinational<ac_int<16, false>> accumulation_read_address;
  Connections::Combinational<BufferVector> accumulation_read_data;
  Connections::Combinational<BufferWriteRequest<BufferVector>> accumulation_write_request;

  BufferVector accumulation_memory[16];
  int read_count;
  int write_count;
  bool test_failed;

  SC_HAS_PROCESS(CIMProcessorTb);

  // Construct the processor and its behavioral accumulation-buffer peer
  explicit CIMProcessorTb(sc_module_name name)
      : sc_module(name),
        dut("dut"),
        clk("clk", 10, SC_NS),
        start_channel("start_channel"),
        read_count(0),
        write_count(0),
        test_failed(false) {
    dut.clk(clk);
    dut.rstn(rstn);
    dut.input_channel(input_channel);
    dut.weight_channel(weight_channel);
    dut.bias_channel(bias_channel);
    dut.params_in(params_channel);
    dut.output_channel(output_channel);
    dut.start(start_channel);
    dut.accumulation_buffer_read_address[0](accumulation_read_address);
    dut.accumulation_buffer_read_data[0](accumulation_read_data);
    dut.accumulation_buffer_write_request[0](accumulation_write_request);

    for (int address = 0; address < 16; address++) {
      accumulation_memory[address] = BufferVector::zero();
    }

    SC_THREAD(run);
    sensitive << clk.posedge_event();

    SC_THREAD(drive_bias);
    sensitive << clk.posedge_event();

    SC_THREAD(run_accumulation_buffer);
    sensitive << clk.posedge_event();
  }

  // Advance one processor cycle
  void tick() {
    wait(clk.posedge_event());
    wait(SC_ZERO_TIME);
  }

  // Record a deterministic failure without abandoning channel cleanup
  void require(bool condition, const std::string &message) {
    if (condition) {
      return;
    }
    std::cerr << "[FAIL] " << message << std::endl;
    test_failed = true;
  }

  // Create the two-address, two-reduction schedule used by this test
  MatrixParams make_params() const {
    MatrixParams params;
    for (int level = 0; level < 2; level++) {
      for (int loop = 0; loop < 6; loop++) {
        params.loops[level][loop] = 1;
      }
    }

    params.x_loop_idx[0] = 1;
    params.y_loop_idx[0] = 0;
    params.weight_loop_idx[0] = 2;
    params.reduction_loop_idx[0] = 4;
    params.fy_loop_idx[0] = 3;

    params.fy_loop_idx[1] = 0;
    params.fx_loop_idx = 1;
    params.y_loop_idx[1] = 2;
    params.weight_loop_idx[1] = 3;
    params.x_loop_idx[1] = 4;
    params.reduction_loop_idx[1] = 5;

    params.loops[1][params.x_loop_idx[1]] = 2;
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.weight_reuse_idx[0] = 0;
    params.weight_reuse_idx[1] = 1;
    params.has_bias = true;
    params.use_input_codebook = false;
    params.use_weight_codebook = false;
    return params;
  }

  // Pack signed activations with one negative lane into the CIM input word
  ac_int<INPUT_BUFFER_WIDTH, false> make_inputs() const {
    ac_int<INPUT_BUFFER_WIDTH, false> inputs = 0;
    for (int row = 0; row < ROWS; row++) {
      const ac_int<A_WIDTH, true> value = row == 0 ? -2 : 1;
      inputs.set_slc(row * A_WIDTH, value.template slc<A_WIDTH>(0));
    }
    return inputs;
  }

  // Pack one deterministic output vector for a resident CIM weight row
  ac_int<WEIGHT_BUFFER_WIDTH, false> make_weights(int operation, int input_row) const {
    ac_int<WEIGHT_BUFFER_WIDTH, false> weights = 0;
    for (int output = 0; output < COLS; output++) {
      const int value = operation + 1 + (output % 3) + input_row;
      weights.set_slc(output * B_WIDTH, ac_int<B_WIDTH, false>(value));
    }
    return weights;
  }

  // Return the expected final value for one output address and lane
  int expected_value(int job, int address, int output) const {
    const int lane_offset = output % 3;
    const int first_operation = job * 4 + address * 2;
    const int input_sum = ROWS - 3;
    const int input_row_sum = ROWS * (ROWS - 1) / 2;
    const int first_partial = input_sum * (first_operation + 1 + lane_offset) + input_row_sum;
    const int second_partial = input_sum * (first_operation + 2 + lane_offset) + input_row_sum;
    return output + first_partial + second_partial;
  }

  // Supply one reusable bias vector for each back-to-back parameter job
  void drive_bias() {
    bias_channel.ResetWrite();
    wait();
    while (!rstn.read()) {
      wait();
    }

    BufferVector bias = BufferVector::zero();
    for (int output = 0; output < COLS; output++) {
      bias[output] = Buffer(output);
    }
    bias_channel.Push(bias);
    bias_channel.Push(bias);
  }

  // Model the persistent MatrixUnit accumulation buffer
  void run_accumulation_buffer() {
    accumulation_read_address.ResetRead();
    accumulation_read_data.ResetWrite();
    accumulation_write_request.ResetRead();
    wait();

    while (true) {
      BufferWriteRequest<BufferVector> write;
      if (accumulation_write_request.PopNB(write)) {
        accumulation_memory[write.address.to_int()] = write.data;
        write_count++;
      }

      ac_int<16, false> address;
      if (accumulation_read_address.PopNB(address)) {
        read_count++;
        accumulation_read_data.Push(accumulation_memory[address.to_int()]);
      }
      wait();
    }
  }

  // Drive one complete matrix operation and check both completed outputs
  void run() {
    params_channel.ResetWrite();
    input_channel.ResetWrite();
    weight_channel.ResetWrite();
    output_channel.ResetRead();
    start_channel.ResetRead();

    rstn.write(false);
    tick();
    tick();
    rstn.write(true);
    tick();

    for (int job = 0; job < 2; job++) {
      params_channel.Push(make_params());
      start_channel.SyncPop();

      for (int operation = 0; operation < 4; operation++) {
        for (int row = 0; row < ROWS; row++) {
          weight_channel.Push(make_weights(job * 4 + operation, row));
        }
        input_channel.Push(make_inputs());
      }

      for (int address = 0; address < 2; address++) {
        const BufferVector result = output_channel.Pop();
        for (int output = 0; output < COLS; output++) {
          std::ostringstream message;
          message << "job " << job << " address " << address << " output " << output << " expected "
                  << expected_value(job, address, output) << " got " << result[output].int_val.to_int();
          require(result[output].int_val.to_int() == expected_value(job, address, output), message.str());
        }
      }
    }

    tick();
    require(write_count == 4, "expected one intermediate write per job and output address");
    require(read_count == 4, "expected one accumulation read per job and output address");

    if (!test_failed) {
      std::cout << "[PASS] cim_processor_int8" << std::endl;
    }
    sc_stop();
  }
};

// Elaborate the focused CIMProcessor v1 test
int sc_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  CIMProcessorTb testbench("cim_processor_int8");
  sc_start();
  return testbench.test_failed ? 1 : 0;
}
