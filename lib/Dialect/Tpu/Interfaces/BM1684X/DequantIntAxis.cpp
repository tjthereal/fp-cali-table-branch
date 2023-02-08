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
#include "tpu_mlir/Support/MathUtils.h"

using namespace tpu_mlir::backend;

// =========================================
// GlobalGenInterface
// =========================================

void tpu::DequantIntAxisOp::codegen_global_bm1684x() {
  dequant_int_param_t param = {0};
  int64_t n, c, h, w;
  module::getNCHW(getInput(), n, c, h, w);
  param.input_addr = module::getAddress(getInput());
  param.dequant_addr = module::getAddress(getQuant());
  param.output_addr = module::getAddress(getOutput());
  param.n = (int)n;
  param.c = (int)c;
  param.h = (int)h;
  param.w = (int)w;

  param.dequant_addr = module::getAddress(getQuant());
  param.is_perchannel = true;
  param.lshift = getLshift();
  param.mode = static_cast<int>(getQuantMode());
  param.input_dtype = BM168x::getDataType(getInput());
  param.output_dtype = BM168x::getDataType(getOutput());
  param.round_mode = getQuantMode() == tpu::DequantMode::Normal
                         ? ROUNDING_HALF_UP
                         : ROUNDING_HALF_AWAY_FROM_ZERO;
  BM168x::call_global_func("backend_api_dequant_int_global", &param,
                           sizeof(param));
}

// =========================================
// LocalGenInterface
// =========================================

int64_t tpu::DequantIntAxisOp::getBufferSize_bm1684x(
    int64_t in_lmem_bytes, int64_t out_lmem_bytes, int64_t in_nslice,
    int64_t in_hslice, int64_t out_nslice, int64_t out_hslice, group_type_t group_type) {
  if (getQuantMode() == DequantMode::TFLite) {
    return in_lmem_bytes;
  }
  return 0;
}

void tpu::DequantIntAxisOp::codegen_local_bm1684x(int64_t n_step,
                                                  int64_t h_step,
                                                  group_type_t group_type,
                                                  local_sec_info_t &sec_info) {
  int64_t n, c, h, w;
  module::getNCHW(getInput(), n, c, h, w, group_type);
  auto gi = getGroupInfo(n_step, h_step);
  auto in_gi = LocalGenInterface::getGroupInfo(getInput(), n_step, h_step);
  auto dequant_gi = LocalGenInterface::getGroupInfo(getQuant(), n_step, h_step);

  dequant_int_param_t param = {0};
  param.input_addr = (uint32_t)in_gi.out_addr;
  param.dequant_addr = (uint32_t)dequant_gi.out_addr;
  param.output_addr = (uint32_t)gi.out_addr;
  param.buffer_local_addr = (uint32_t)gi.buffer_addr;
  param.n = sec_info.out_n_slice;
  param.c = c;
  param.h = sec_info.out_h_slice;
  param.w = sec_info.out_w_slice;
  param.is_perchannel = true;
  param.input_dtype = BM168x::getDataType(getInput());
  param.output_dtype = BM168x::getDataType(getOutput());
  param.lshift = getLshift();
  param.mode = static_cast<int>(getQuantMode());
  param.round_mode = getQuantMode() == tpu::DequantMode::Normal
                         ? ROUNDING_HALF_UP
                         : ROUNDING_HALF_AWAY_FROM_ZERO;

  BM168x::call_local_func("backend_api_dequant_int_local", &param,
                          sizeof(param));
}

// dynamic codegen
int64_t tpu::DequantIntAxisOp::dyn_codegen_local_bm1684x(void *buffer) {
  return 0;
}

// ======================================
// Dynamic GlobalGenInterface
// ======================================
int64_t tpu::DequantIntAxisOp::dyn_codegen_global_bm1684x(void *buffer) {
  return 0;
}
