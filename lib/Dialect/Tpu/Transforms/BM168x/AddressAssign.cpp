//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// TPU-MLIR is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "tpu_mlir/Dialect/Tpu/Transforms/Passes.h"
#include "tpu_mlir/Dialect/Tpu/IR/TpuOps.h"
#include "tpu_mlir/Support/MathUtils.h"
#include "tpu_mlir/Support/Helper/Module.h"
#include "tpu_mlir/Support/Helper/Quant.h"
#include "tpu_mlir/Backend/BM168x/BM1684.h"
#include "tpu_mlir/Backend/BM168x/BM1684x.h"
#include "tpu_mlir/Dialect/Tpu/Transforms/CV18xx/GmemAllocator.hpp"

#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/Dialect/Quant/QuantTypes.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"

#include <sstream>
#include <fstream>
#include <set>
#include <tuple>
#include <vector>

using namespace llvm;
using namespace mlir;
using namespace tpu_mlir::helper;
using namespace tpu_mlir::backend;
namespace tpu_mlir {
namespace tpu {

class AddressAssignPass : public AddressAssignBase<AddressAssignPass> {
public:
  AddressAssignPass() {}
  void runOnOperation() override {
    auto module = getOperation();
    auto state = Module::getState(module);
    if (state != Module::State::TPU_DIVIDED) {
      llvm_unreachable("module should be divided");
    }
    Module::removeUnusedOp(module);
    bm_assign_addr(module);
  }

protected:
  void bm_assign_addr(mlir::ModuleOp &module) {
    int64_t start_addr = 0;
    int64_t alignment = BM168x::ALIGNMENT;
    //bool check = false;
    chip = Module::getChip(module);
    if (chip == Module::Chip::BM1684) {
      start_addr = BM1684::instance().get_ctx_start_addr();
    } else if (chip == Module::Chip::BM1684x) {
      start_addr = BM1684x::instance().get_ctx_start_addr();
    } else {
      llvm_unreachable("chip not support now");
    }
    Builder builder(module.getContext());
    // assign weight first
    auto addr = start_addr;
    for (auto func : module.getOps<FuncOp>()) {
      func.walk([&](top::WeightOp op) {
        Module::setAddress(op.output(), addr);
        int64_t bytes = Module::getBytes(op.output());
        addr = align_up(addr + bytes, alignment);
      });
    }
    Module::setCoeffAddr(module, start_addr);
    Module::setCoeffSize(module, addr - start_addr);
    // assign activation
    start_addr = addr;
    uint32_t loc = 0;
    //key: the operation pointer + output index, convert the result to type int64_t
    std::map<int64_t, TensorLive> liveRange;
    std::map<Operation *, uint32_t> ops_loc;
    std::vector<int64_t> common_ops;
    std::vector<int64_t> inplace_ops;
    //0.update liverange of ops and choose ops to allocate.
    for (auto func : module.getOps<FuncOp>()) {
      func.walk([&](Operation *op) {
        ops_loc[op] = loc;
        ++loc;
      });
    }
    for (auto func : module.getOps<FuncOp>()) {
      func.walk([&](Operation *op) {
        int n = op->getNumResults();
        for (int i = 0; i < n; i++) {
          updateLiveRangeofBMOps(op, i, ops_loc, liveRange, common_ops, inplace_ops, alignment);
        }
      }
      );
    }
    //1.assign common_ops
    //key: the operation pointer + output index, convert the result to type int64_t
    std::map<int64_t, int64_t> gaddrMap;
    if (!common_ops.empty()) {
      GmemAllocator allocator(gaddrMap, alignment);
      auto gmemUsed = allocator.assignGaddr(common_ops, liveRange, true, start_addr);
      addr += gmemUsed;
    }
    for (auto func : module.getOps<FuncOp>()) {
      func.walk([&](Operation *op) {
        for (int i = 0; i < op->getNumResults(); i++) {
          int64_t save_op = (int64_t)op + i;
          if (gaddrMap.find(save_op) != gaddrMap.end()) {
            Module::setAddress(op->getResult(i), gaddrMap[save_op]);
          }
        }
      });
      func.walk([&](tpu::GroupOp gOp) {
        int idx = 0;
        gOp.body().walk([&](tpu::StoreOp sOp) {
          auto addr = Module::getAddress(gOp.getResult(idx));
          Module::setAddress(sOp.output(), addr);
          idx++;
        });
      });
    }
    //2.set inplace_ops address
    for (auto op : inplace_ops) {
    if (auto reshapeOp = dyn_cast<tpu::ReshapeOp>((Operation *)op)) {
      if (chip == Module::Chip::BM1684x) {
        auto addr = Module::getAddress(reshapeOp.input());
        Module::setAddress(reshapeOp.output(), addr);
        }
      }
    }
    Module::setNeuronAddr(module, start_addr);
    Module::setNeuronSize(module, addr - start_addr);
    Module::updateModuleTypes(module);
    Module::setState(module, Module::State::TPU_ADDRESSED);
  }

  void updateLiveRangeofBMOps(Operation* op, int index, std::map<Operation *, uint32_t> &ops_loc,
        std::map<int64_t, TensorLive> &liveRange,
        std::vector<int64_t> &common_ops, std::vector<int64_t> &inplace_ops, int alignment) {
    auto updateOperandsLiveRange = [&](Operation *op, uint32_t endPosition) {
      for (uint32_t i = 0; i < op->getNumOperands(); i++) {
        auto operand = op->getOperand(i);
        auto opd = operand.getDefiningOp();
        if (opd == 0x0) {
          continue;
        }
        int64_t save_opd = (int64_t)opd;
        if (opd->getNumResults() > 1) {
          int this_index = getOutIndex(opd, operand);
          assert(this_index != -1);
          save_opd = save_opd + this_index;
        }
        if (liveRange.find(save_opd) != liveRange.end()) {
          if (isa<top::InputOp>(opd) && liveRange[save_opd].end == 0xFFFFFFFF) {
            continue;
          }
          if (liveRange[save_opd].end == 0xFFFFFFFF ||
              liveRange[save_opd].end < endPosition) {
            liveRange[save_opd].end = endPosition;
          }
        }
      }
    };
    int64_t save_op = (int64_t)op + index;
    uint32_t loc = ops_loc[op];
    uint32_t endPosition = loc + 1;
    if (isa<top::InputOp>(op)) {
      //liveRange.emplace_back(TensorLive(out, 0, 0xFFFFFFFF));
      uint32_t tensor_size = getTensorGmemSize(op, index, alignment);
      assert(liveRange.count(save_op) == 0);
      liveRange[save_op] = TensorLive(index, 0, 0xFFFFFFFF, tensor_size);
      updateOperandsLiveRange(op, endPosition);
      common_ops.emplace_back(save_op);
    } else if (isa<FuncOp, top::NoneOp, func::ReturnOp, top::WeightOp,
                func::CallOp, tpu::YieldOp>(op) || Module::isOpInGroup(op)) {
      updateOperandsLiveRange(op, endPosition);
    } else if (isInPlaceOp(op)) {
      uint32_t maxPosition = endPosition;
      findInPlaceOpMaxUsePosition(op, maxPosition, ops_loc);
      updateOperandsLiveRange(op, maxPosition);
      inplace_ops.emplace_back(save_op);
    } else if (op->getDialect()->getNamespace() == "tpu") {
      uint32_t tensor_size = getTensorGmemSize(op, index, alignment);
      assert(liveRange.count(save_op) == 0);
      liveRange[save_op] = TensorLive(index, loc, 0xFFFFFFFF, tensor_size);
      updateOperandsLiveRange(op, endPosition);
      common_ops.emplace_back(save_op);
    } else {
      updateOperandsLiveRange(op, endPosition);
    }
  }

  void findInPlaceOpMaxUsePosition(Operation *op, uint32_t &maxPosition, std::map<Operation *, uint32_t> &ops_loc) {
    for (auto &use : op->getResult(0).getUses()) {
      Operation *next = use.getOwner();
      if (isInPlaceOp(next)) {
        findInPlaceOpMaxUsePosition(next, maxPosition, ops_loc);
      } else {
        uint32_t curPosition = ops_loc[next] + 1;
        if (maxPosition < curPosition) {
          maxPosition = curPosition;
        }
      }
    }
  }

  bool isInPlaceOp(Operation *op) {
    // TODO crop op
    if (isa<tpu::ReshapeOp>(op)) {
      return true;
    }
    return false;
  }

  int getOutIndex(Operation *op, Value &out) {
    for (int i = 0; i < op->getNumResults(); i++) {
      if (op->getResult(i) == out) {
        return i;
      }
    }
    return -1;
  }

  uint32_t getTensorGmemSize(Operation *op, int index, int64_t aligment_) {
    uint32_t size = Module::getBytes(op->getResult(index));
    // pad to aligment_
    if (size % aligment_) {
      size = size + aligment_ - (size % aligment_);
    }
    return size;
}

  StringRef chip;
};

std::unique_ptr<OperationPass<ModuleOp>> createAddressAssignPass() {
  return std::make_unique<AddressAssignPass>();
}
} // namespace tpu
} // namespace tpu_mlir