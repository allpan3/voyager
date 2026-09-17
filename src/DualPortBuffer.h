#pragma once

#include <ac_shared.h>
#include <mc_connections.h>
#include <systemc.h>

// Keep the SRAM read and write ports independently backpressured
template <typename T, int size>
SC_MODULE(DualPortBuffer) {
 private:
  static const unsigned int NUM_BANKS = DOUBLE_BUFFERED_ACCUM_BUFFER ? 2 : 1;
  static const unsigned int NUM_PORTS_PER_BANK =
      DOUBLE_BUFFERED_ACCUM_BUFFER ? 2 : 1;
  ac_shared<T[size]> bank0;
#if DOUBLE_BUFFERED_ACCUM_BUFFER
  ac_shared<T[size]> bank1;
  Connections::SyncChannel handoff[NUM_BANKS];
#endif

 public:
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);
  Connections::In<ac_int<16, false>>
      read_address[NUM_BANKS * NUM_PORTS_PER_BANK];
  Connections::Out<T> read_data[NUM_BANKS * NUM_PORTS_PER_BANK];
  Connections::In<BufferWriteRequest<T>>
      write_request[NUM_BANKS * NUM_PORTS_PER_BANK];
#if DOUBLE_BUFFERED_ACCUM_BUFFER
  Connections::SyncIn done[NUM_BANKS * NUM_PORTS_PER_BANK];
#endif

  // Give each physical SRAM port its own process and stall condition
  SC_CTOR(DualPortBuffer) {
    SC_THREAD(bank0_write);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
    SC_THREAD(bank0_read);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
#if DOUBLE_BUFFERED_ACCUM_BUFFER
    SC_THREAD(bank1_write);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
    SC_THREAD(bank1_read);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
#endif
  }

  // Drain the old owner's responses before acknowledging a bank handoff
  template <int bank>
  void read_bank() {
#pragma hls_unroll yes
    for (int p = 0; p < NUM_PORTS_PER_BANK; ++p) {
      read_address[bank * NUM_PORTS_PER_BANK + p].Reset();
      read_data[bank * NUM_PORTS_PER_BANK + p].Reset();
    }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
    handoff[bank].ResetRead();
#endif
    unsigned int port = bank * NUM_PORTS_PER_BANK;
    wait();
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      while (!handoff[bank].SyncPopNB())
#endif
      {
        ac_int<16, false> address;
        if (read_address[port].PopNB(address)) {
#ifndef __SYNTHESIS__
          if (address > size) throw std::runtime_error("Address out of bounds");
#endif
          T value;
          if constexpr (bank == 0) {
            value = bank0[address];
          }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
          else {
            value = bank1[address];
          }
#endif
          read_data[port].Push(value);
        }
#ifndef __SYNTHESIS__
        wait();
#endif
      }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      port ^= 1;
#endif
    }
  }

  // Accept writes independently and synchronize both ports at bank handoff
  template <int bank>
  void write_bank() {
#pragma hls_unroll yes
    for (int p = 0; p < NUM_PORTS_PER_BANK; ++p) {
      write_request[bank * NUM_PORTS_PER_BANK + p].Reset();
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      done[bank * NUM_PORTS_PER_BANK + p].Reset();
#endif
    }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
    handoff[bank].ResetWrite();
#endif
    unsigned int port = bank * NUM_PORTS_PER_BANK;
    wait();
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      while (!done[port].SyncPopNB())
#endif
      {
        BufferWriteRequest<T> request;
        if (write_request[port].PopNB(request)) {
#ifndef __SYNTHESIS__
          if (request.address > size)
            throw std::runtime_error("Address out of bounds");
#endif
          if constexpr (bank == 0) {
            bank0[request.address] = request.data;
          }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
          else {
            bank1[request.address] = request.data;
          }
#endif
        }
#ifndef __SYNTHESIS__
        wait();
#endif
      }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      handoff[bank].SyncPush();
      port ^= 1;
#endif
    }
  }

  // Bind each port process to one shared SRAM bank
  void bank0_read() { read_bank<0>(); }
  // Bind the independent write process to bank zero
  void bank0_write() { write_bank<0>(); }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
  // Bind the read process to bank one
  void bank1_read() { read_bank<1>(); }
  // Bind the independent write process to bank one
  void bank1_write() { write_bank<1>(); }
#endif
};
