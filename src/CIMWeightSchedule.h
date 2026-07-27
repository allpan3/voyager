#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

// CIMWeightGroup describes one resident bank group and its logical replays
struct CIMWeightGroup {
  ac_int<16, false> set_count;
  ac_int<16, false> replay_count;

  static const unsigned int width = 32;

  template <unsigned int Size>
  void Marshall(Marshaller<Size> &m) {
    m & set_count;
    m & replay_count;
  }

  inline friend void sc_trace(sc_trace_file *tf, const CIMWeightGroup &group,
                              const std::string &name) {
    sc_trace(tf, group.set_count, name + ".set_count");
    sc_trace(tf, group.replay_count, name + ".replay_count");
  }

  inline friend std::ostream &operator<<(std::ostream &os,
                                         const CIMWeightGroup &group) {
    os << group.set_count << " " << group.replay_count;
    return os;
  }

  inline friend bool operator==(const CIMWeightGroup &lhs,
                                const CIMWeightGroup &rhs) {
    return lhs.set_count == rhs.set_count &&
           lhs.replay_count == rhs.replay_count;
  }
};
