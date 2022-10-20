/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <utility>

#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/IR/Linalg.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/transforms/cpu/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/gml_st/IR/gml_st_ops.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/gml_st/transforms/tiling.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/gml_st/transforms/tiling_interface_impl.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/gml_st/transforms/transforms.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/thlo/IR/thlo_ops.h"

namespace mlir::cpu {
namespace {

#define GEN_PASS_DEF_TRANSFORMMATMULFORCPUPASS
#include "tensorflow/compiler/xla/mlir/transforms/cpu/passes.h.inc"

struct TransformMatmulForCpuPass
    : public impl::TransformMatmulForCpuPassBase<TransformMatmulForCpuPass> {
  TransformMatmulForCpuPass() = default;
  explicit TransformMatmulForCpuPass(
      llvm::ArrayRef<int64_t> matmul_tile_sizes) {
    tile_sizes = matmul_tile_sizes;
  }

  void getDependentDialects(DialectRegistry &registry) const final {
    registry.insert<mlir::gml_st::GmlStDialect, arith::ArithDialect,
                    linalg::LinalgDialect, tensor::TensorDialect>();
    mlir::gml_st::registerGmlStTilingInterfaceExternalModels(registry);
  }

  void runOnOperation() override {
    func::FuncOp f = getOperation();
    MLIRContext *ctx = &getContext();

    mlir::gml_st::TilingOptions opts;

    if ((*tile_sizes).empty()) {
      tile_sizes = {2, 2, 2};
    }

    assert(tile_sizes.size() == 3 &&
           "Tiling sizes for MatMul should have 3 elements");

    auto filter_fn = [&](Operation *op) {
      return success(isa<mlir::linalg::MatmulOp>(op));
    };

    ///////////////////////////////
    // Tiling parallel dimensions
    opts.setTileSizeComputationFn({(*tile_sizes)[0], (*tile_sizes)[1], 0});

    RewritePatternSet patterns(ctx);
    populateTilingPatterns(ctx, filter_fn, opts, &patterns);

    if (failed(applyPatternsAndFoldGreedily(f, std::move(patterns)))) {
      return signalPassFailure();
    }

    // Ensure we drop the marker in the end.
    f.walk([](linalg::LinalgOp op) { gml_st::removeTransformationAttr(op); });

    ///////////////////////////////
    // Tiling reduction dimension
    opts.setTileSizeComputationFn({0, 0, (*tile_sizes).back()});
    opts.distribute = false;

    RewritePatternSet newpatterns(ctx);
    populateTilingPatterns(ctx, filter_fn, opts, &newpatterns);

    if (failed(applyPatternsAndFoldGreedily(f, std::move(newpatterns)))) {
      return signalPassFailure();
    }

    // Ensure we drop the marker in the end.
    f.walk([](linalg::LinalgOp op) { gml_st::removeTransformationAttr(op); });
  }
};

}  // namespace
}  // namespace mlir::cpu

namespace xla::cpu {

std::unique_ptr<mlir::OperationPass<mlir::func::FuncOp>>
createTransformMatmulForCpuPass() {
  return std::make_unique<mlir::cpu::TransformMatmulForCpuPass>();
}

std::unique_ptr<mlir::OperationPass<mlir::func::FuncOp>>
createTransformMatmulForCpuPass(llvm::ArrayRef<int64_t> matmul_tile_sizes) {
  return std::make_unique<mlir::cpu::TransformMatmulForCpuPass>(
      matmul_tile_sizes);
}

}  // namespace xla::cpu
