/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef JITTERDATASTRUCT_
#define JITTERDATASTRUCT_

#include <bitset>
#include <optional>
#include <stdint.h>

#include "llvm/Support/JSON.h"

namespace vISA {

struct VISA_BB_INFO {
  int id;
  unsigned staticCycle;
  unsigned sendStallCycle;
  unsigned char loopNestLevel;
};

// PERF_STATS_CORE - the core vISA static performance stats
// This set of stats may be used not only for stats report, but for
// other purposes such as spill cost estimation by IGC.
struct PERF_STATS {
public:
  // Hash value of the binary. Used by stats report.
  uint64_t binaryHash = 0;

  // Number of GRF acutally being used. Stats collection only.
  uint32_t numGRFUsed = 0;

  // Number of configured threads and GRF number. Used by IGC for
  // setting execution environment in output.
  uint32_t numGRFTotal = 0;
  uint32_t numThreads = 0;

  // Un-weighted asm instructions count. Used by IGC for spill
  // cost calculation
  uint32_t numAsmCountUnweighted = 0;

  // Number of flag spill and fill. Used by VC Stats
  uint32_t numFlagSpillStore = 0;
  uint32_t numFlagSpillLoad = 0;

  // Number of spill/fill, weighted by loop. Used by IGC for
  // spill cost calculation.
  uint32_t numGRFSpillFillWeighted = 0;

  // spillMemUsed is the scratch size in byte of entire vISA stack for this
  // function/kernel. It contains spill size and caller/callee save size.
  uint32_t spillMemUsed = 0;

  // Unweighted cycles count estimated by the scheduler.
  uint32_t numCycles = 0;

  uint32_t maxGRFPressure = 0;

  // These fields are currently used by IGC.
  // The first two are unweighted (i.e., just a sum of each basic block's
  // estimated cycles), while the last two are weighted by loop (16 iterations
  // per loop).
  // Note that these stats are valid only if post-RA scheduling is enabled.
  uint32_t sendStallCycle = 0;
  uint32_t staticCycle = 0;
  uint32_t loopNestedStallCycle = 0;
  uint32_t loopNestedCycle = 0;

public:
  llvm::json::Value toJSON();
};

// PERF_STATS_VERBOSE - the verbose vISA static performance stats.
// This set of stats are used/set only when the verbose stats are
// queried (vISA_DumpPerfStatsVerbose)
// TODO: This set will be disable completely in the Release build.
struct PERF_STATS_VERBOSE {
  // The number of bank conflict.
  unsigned BCNum = 0;

  // counting the number of read-modify-write
  unsigned numRMWs = 0;

  // For the static profiling of acc regsiter substituion ratio
  // ALU instruction number
  unsigned numALUInst = 0;
  // ALU instruction destination operand number, which is not used in non-ALU
  // instruction
  unsigned numALUOnlyDst = 0;
  // ALU instruction source operand number, which is not defined in non-ALU
  // instruction
  unsigned numALUOnlySrc = 0;

  // The number of the operand which uses acc register
  // Def:dst operand, Use:src operand
  unsigned accSubDef = 0;
  unsigned accSubUse = 0;

  // Candidates, which may be substituted with acc, or not because of spill
  unsigned accSubCandidateDef = 0;
  unsigned accSubCandidateUse = 0;

  // The number of sync instructions.
  unsigned syncInstCount = 0;
  // The token reuse times
  unsigned tokenReuseCount = 0;
  // The number of @1 distance in single ALU pipeline, it can be L@1,
  // I@1, F@1 or @1 of TGL.
  unsigned singlePipeAtOneDistNum = 0;
  // A@1 number
  unsigned allAtOneDistNum = 0;
  // The number of $x.dst, after write token dependence
  unsigned AfterWriteTokenDepCount = 0;
  // The number of $x.src, after read token dependence
  unsigned AfterReadTokenDepCount = 0;

public:
  llvm::json::Value toJSON();
};

struct FINALIZER_INFO {
  // ----- Required by IGC/VC/Runtime ----- //
  // Used by IGC for spill cost calculation
  bool isSpill = false;

  // Debug info is callee allocated and populated only if switch is passed
  // to JIT to emit debug info.
  void *genDebugInfo = nullptr;
  uint32_t genDebugInfoSize = 0;

  // Propagate information about barriers presence back to IGC. It's safer to
  // depend on vISA statistics as IGC is not able to detect barriers if they
  // are used as a part of Inline vISA code.
  // This information is used by legacy CMRT as well as OpenCL/L0 runtime.
  uint32_t numBarriers = 0;

  // Number of basic blocks in the kernel, used by IGC for stat reporting.
  uint32_t BBNum = 0;
  // TODO: this is no longer used, can we remove them without breaking stuff?
  VISA_BB_INFO *BBInfo = nullptr;

  // Whether kernel recompilation should be avoided. vISA hint for IGC.
  bool avoidRetry = false;

  // GTPin information
  void *freeGRFInfo = nullptr;
  uint32_t freeGRFInfoSize = 0;
  unsigned char numBytesScratchGtpin = 0;

  // Used by VC for setting execution environment in output
  bool hasStackcalls = false;

  // load-thread-payload prologs offset required by runtime
  // for skipping the prologs
  uint32_t offsetToSkipPerThreadDataLoad = 0;
  uint32_t offsetToSkipCrossThreadDataLoad = 0;

  // When two entries prolog is added for setting FFID
  // for compute (GP or GP1), skip this offset to set FFID_GP1.
  // Will set FFID_GP if not skip
  uint32_t offsetToSkipSetFFIDGP = 0;
  uint32_t offsetToSkipSetFFIDGP1 = 0;

  // ----- vISA Stats ----- //
  PERF_STATS stats;
  PERF_STATS_VERBOSE statsVerbose;
};

} // namespace vISA
#endif // JITTERDATASTRUCT_
