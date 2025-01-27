/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef _STATICPROFILING_H
#define _STATICPROFILING_H

#include "../BuildIR.h"
#include "../G4_IR.hpp"
#include "../FlowGraph.h"

namespace vISA {

class StaticProfiling {
  IR_Builder &builder;
  G4_Kernel &kernel;

public:
  StaticProfiling(IR_Builder &B, G4_Kernel &K)
      : builder(B), kernel(K) {}

  StaticProfiling(const StaticProfiling &) = delete;
  virtual ~StaticProfiling() = default;

  void ALUInstructionProfile(G4_INST *inst);

  void run() {
    for (auto bb : kernel.fg) {
      for (auto inst : *bb) {
        ALUInstructionProfile(inst);
      }
    }
  }

};

} // namespace vISA

#endif // _STATICPROFILING_H
