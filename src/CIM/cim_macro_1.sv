// CIM macro 1 implementation and adapter for CIMIntMacroWrapper
`include "cim_typedefs.svh"

/* verilator lint_off WIDTHEXPAND */
/* verilator lint_off WIDTHTRUNC */
// CIMVanillaMacro models the concrete bit-serial SRAM-backed macro used by CIM macro 1
module CIMVanillaMacro #(
    parameter int unsigned CIN  = 256,      //MAC input dimension
    parameter int unsigned COUT = 64,       //MAC output dimension
    parameter int unsigned W_BITS        = 4,
    parameter int unsigned A_BITS        = 4,
    parameter int unsigned DIN_BITS     = COUT*W_BITS,
    parameter int unsigned ADD_BITS     = $clog2(CIN) + W_BITS,
    parameter int unsigned PSUM_BITS    = ADD_BITS + A_BITS,
    parameter int unsigned DOUT_BITS    = PSUM_BITS * COUT
)(
    input wire [CIN-1:0] IN,      //bit-serial input activations
    input wire [DIN_BITS-1:0] DIN,     //SRAM write bit-width
    input wire [$clog2(CIN)-1:0]   WADDR,
    input wire CLK,
    input wire WEN,               //Write enable, low active
    input wire CEN,               //Compute enable, low active
    output reg [DOUT_BITS-1:0] DOUT
);
    logic [$clog2(A_BITS+1)-1:0] bit_cnt;

    logic [DIN_BITS-1:0] WSRAM [CIN];
    logic [DOUT_BITS-1:0] PSUM;
    always @ (posedge CLK) begin
        if (~WEN) begin
            WSRAM[WADDR] <= DIN;
        end
    end

    always @ (posedge CLK) begin
        if (~CEN) begin
            bit_cnt <= (bit_cnt == A_BITS)? 1: bit_cnt + 1;
        end
        else begin
            bit_cnt <= A_BITS;
        end
    end
        
    logic [ADD_BITS-1:0] ADD_RES [COUT];
    genvar gv_i;
    generate
    for (gv_i = 0; gv_i < COUT; gv_i++) begin : g_add
        always_comb begin
            ADD_RES[gv_i] = '0;
            for (int k = 0; k < CIN; k++) begin
                if (IN[k]) begin
                    ADD_RES[gv_i] += WSRAM[k][gv_i*W_BITS +: W_BITS];   //assume unsigned addition for simplicity
                end
            end
        end
    end
    endgenerate

    generate
    for (gv_i = 0; gv_i < COUT; gv_i++) begin : g_dout
        always @ (posedge CLK) begin
            if (~CEN) begin
                PSUM[gv_i*PSUM_BITS +: PSUM_BITS] <= 
                    (bit_cnt == A_BITS)? 
                        ADD_RES[gv_i] << (A_BITS-1): 
                        (ADD_RES[gv_i] << (A_BITS-1)) + 
                        (PSUM[gv_i*PSUM_BITS +: PSUM_BITS] >> 1);
			DOUT[gv_i*PSUM_BITS +: PSUM_BITS] <= (bit_cnt == A_BITS)? PSUM[gv_i*PSUM_BITS +: PSUM_BITS]: DOUT[gv_i*PSUM_BITS +: PSUM_BITS];
            end
        end
    end
    endgenerate
endmodule
/* verilator lint_on WIDTHTRUNC */
/* verilator lint_on WIDTHEXPAND */


// CIMVanillaMacroAdapter maps the canonical wrapper interface toward a generic real macro shape
module CIMVanillaMacroAdapter #(
    parameter int unsigned CH_IN = 64,
    parameter int unsigned CH_OUT = 8,
    parameter int unsigned NUM_ROWS = 1,
    parameter int unsigned A_WIDTH = 4,
    parameter int unsigned B_WIDTH = 4,
    parameter int unsigned C_WIDTH = 20,
    parameter int unsigned WRITE_BW = 1,
    parameter int unsigned MAC_LATENCY = 1,
    parameter cim_mode_t MODE = CIM_MODE_BIT_SERIAL,
    localparam int unsigned BITS_CH_IN = (CH_IN <= 1) ? 1 : $clog2(CH_IN),
    localparam int unsigned BITS_ROW = (NUM_ROWS <= 1) ? 1 : $clog2(NUM_ROWS)
) (
    input  logic                  wclk,
    input  logic                  mclk,
    input  logic [A_WIDTH-1:0]    a [CH_IN],
    input  logic [B_WIDTH-1:0]    b [CH_OUT][WRITE_BW],
    input  logic                  wen,
    input  logic                  mac,
    input  logic                  init,
    input  logic                  a_signed,
    input  logic                  b_signed [CH_OUT],
    input  logic [BITS_CH_IN-1:0] widx,
    input  logic [BITS_ROW-1:0]   wrow,
    input  logic [BITS_ROW-1:0]   mrow,
    output logic [C_WIDTH-1:0]    c [CH_OUT]
);

  localparam int unsigned GENERIC_DIN_BITS = CH_OUT * B_WIDTH;
  localparam int unsigned GENERIC_ADD_BITS = $clog2(CH_IN) + B_WIDTH;
  localparam int unsigned GENERIC_PSUM_BITS = GENERIC_ADD_BITS + A_WIDTH;
  localparam int unsigned GENERIC_DOUT_BITS = GENERIC_PSUM_BITS * CH_OUT;
  localparam int unsigned BITS_A_BIT_IDX = (A_WIDTH <= 1) ? 1 : $clog2(A_WIDTH);
  localparam logic [BITS_A_BIT_IDX-1:0] A_LAST_IDX = BITS_A_BIT_IDX'(A_WIDTH - 1);

  logic [BITS_A_BIT_IDX-1:0] active_bit_idx;
  logic [BITS_A_BIT_IDX-1:0] bit_idx;
  logic flush_pending;
  logic internal_compute;
  logic [CH_IN-1:0] generic_in;
  logic [GENERIC_DIN_BITS-1:0] generic_din;
  logic [GENERIC_DOUT_BITS-1:0] generic_dout;
  logic [BITS_CH_IN-1:0] generic_waddr;
  logic generic_clk;
  logic generic_wen_n;
  logic generic_cen_n;

  assign active_bit_idx = (init && mac) ? '0 : bit_idx;
  assign internal_compute = mac || flush_pending;
  assign generic_clk = mclk | wclk;  // Vanilla macro has one shared write/compute clock
  assign generic_waddr = widx;
  assign generic_wen_n = ~wen;
  assign generic_cen_n = ~internal_compute;

  initial begin
    bit_idx = '0;
    flush_pending = 1'b0;
  end

  always_ff @(posedge mclk) begin
    if (mac) begin
      if (init) begin
        if (A_WIDTH > 1) begin
          bit_idx <= BITS_A_BIT_IDX'(1);
        end else begin
          bit_idx <= '0;
        end
        flush_pending <= (A_WIDTH == 1);
      end else if (bit_idx == A_LAST_IDX) begin
        bit_idx <= '0;
        flush_pending <= 1'b1;
      end else begin
        bit_idx <= bit_idx + BITS_A_BIT_IDX'(1);
        flush_pending <= 1'b0;
      end
    end else if (flush_pending) begin
      bit_idx <= '0;
      flush_pending <= 1'b0;
    end else begin
      flush_pending <= 1'b0;
    end
  end

  always_comb begin
    generic_in = '0;
    for (int chi = 0; chi < CH_IN; chi++) begin
      if (mac) begin
        generic_in[chi] = a[chi][active_bit_idx];
      end
    end
  end

  always_comb begin
    generic_din = '0;
    for (int cho = 0; cho < CH_OUT; cho++) begin
      generic_din[cho*B_WIDTH +: B_WIDTH] = b[cho][0];
    end
  end

  CIMVanillaMacro #(
      .CIN(CH_IN),
      .COUT(CH_OUT),
      .W_BITS(B_WIDTH),
      .A_BITS(A_WIDTH)
  ) cim_macro (
      .IN(generic_in),
      .DIN(generic_din),
      .WADDR(generic_waddr),
      .CLK(generic_clk),
      .WEN(generic_wen_n),
      .CEN(generic_cen_n),
      .DOUT(generic_dout)
  );

  always_comb begin
    for (int cho = 0; cho < CH_OUT; cho++) begin
      c[cho] = C_WIDTH'(generic_dout[cho*GENERIC_PSUM_BITS +: GENERIC_PSUM_BITS]);
    end
  end

`ifndef SYNTHESIS
  initial begin
    if (NUM_ROWS != 1) begin
      $fatal(1, "CIMVanillaMacroAdapter: CIM macro 1 adapter requires NUM_ROWS == 1");
    end
    if (WRITE_BW != 1) begin
      $fatal(1, "CIMVanillaMacroAdapter: CIM macro 1 adapter requires WRITE_BW == 1");
    end
    if (MODE != CIM_MODE_BIT_SERIAL) begin
      $fatal(1, "CIMVanillaMacroAdapter: CIM macro 1 adapter requires bit-serial mode");
    end
    if (MAC_LATENCY < 3) begin
      $fatal(1, "CIMVanillaMacroAdapter: CIM macro 1 adapter requires MAC_LATENCY >= 3");
    end
  end

  // Signed canonical controls are unsupported by the generic macro shape
  always @(posedge mclk) begin
    if (mac && a_signed) begin
      $error("CIMVanillaMacroAdapter: a_signed must stay low");
    end
    for (int cho = 0; cho < CH_OUT; cho++) begin
      if (mac && b_signed[cho]) begin
        $error("CIMVanillaMacroAdapter: b_signed[%0d] must stay low", cho);
      end
    end
    if (mac && (mrow != '0)) begin
      $error("CIMVanillaMacroAdapter: mrow must stay zero because CIM macro 1 has no row banking");
    end
  end

  // The generic macro has one CLK, so write traffic is assumed synchronous to mclk
  always @(posedge wclk) begin
    if (wen && (wrow != '0)) begin
      $error("CIMVanillaMacroAdapter: wrow must stay zero because CIM macro 1 has no row banking");
    end
  end
`endif

endmodule
