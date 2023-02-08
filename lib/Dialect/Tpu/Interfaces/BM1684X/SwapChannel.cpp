//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Backend/BM168x/BM1684X.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"

#include "tpu_mlir/Support/Module.h"

using namespace tpu_mlir::backend;

void tpu::SwapChannelOp::codegen_global_bm1684x() {
  llvm_unreachable("Not Implemented");
}

int64_t tpu::SwapChannelOp::dyn_codegen_global_bm1684x(void *buffer) {
  return 0;
}
// =========================================
// LocalGenInterface
// =========================================

int64_t tpu::SwapChannelOp::getBufferSize_bm1684x(
    int64_t in_lmem_bytes, int64_t out_lmem_bytes, int64_t in_nslice,
    int64_t in_hslice, int64_t out_nslice, int64_t out_hslice,
    group_type_t group_type) {
  return 0;
}

void tpu::SwapChannelOp::codegen_local_bm1684x(int64_t n_step, int64_t h_step,
                                               group_type_t group_type,
                                               local_sec_info_t &sec_info) {
  llvm_unreachable("Not Implemented");
}
