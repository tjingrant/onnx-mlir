//===-------------- LowerKrnl.cpp - Krnl Dialect Lowering -----------------===//
//
// Copyright 2019-2020 The IBM Research Authors.
//
// =============================================================================
//
//
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/LoopUtils.h"

#include "src/Dialect/Krnl/KrnlOps.hpp"
#include "src/Pass/Passes.hpp"

using namespace mlir;

namespace {

void lowerIterateOp(KrnlIterateOp &iterateOp, OpBuilder &rewriter,
    SmallVector<std::pair<Value, AffineForOp>, 4> &nestedForOps) {
  rewriter.setInsertionPointAfter(iterateOp);
  SmallVector<std::pair<Value, AffineForOp>, 4> currentNestedForOps;
  auto boundMapAttrs =
      iterateOp.getAttrOfType<ArrayAttr>(KrnlIterateOp::getBoundsAttrName())
          .getValue();
  auto operandItr =
      iterateOp.operand_begin() + iterateOp.getNumOptimizedLoops();
  for (size_t boundIdx = 0; boundIdx < boundMapAttrs.size(); boundIdx += 2) {
    // Consume input loop operand, currently do not do anything with it.
    auto unoptimizedLoopRef = *(operandItr++);

    // Organize operands into lower/upper bounds in affine.for ready formats.
    llvm::SmallVector<Value, 4> lbOperands, ubOperands;
    AffineMap lbMap, ubMap;
    for (int boundType = 0; boundType < 2; boundType++) {
      auto &operands = boundType == 0 ? lbOperands : ubOperands;
      auto &map = boundType == 0 ? lbMap : ubMap;
      map =
          boundMapAttrs[boundIdx + boundType].cast<AffineMapAttr>().getValue();
      operands.insert(
          operands.end(), operandItr, operandItr + map.getNumInputs());
      std::advance(operandItr, map.getNumInputs());
    }
    currentNestedForOps.emplace_back(std::make_pair(
        unoptimizedLoopRef, rewriter.create<AffineForOp>(iterateOp.getLoc(),
                                lbOperands, lbMap, ubOperands, ubMap)));

    rewriter.setInsertionPoint(currentNestedForOps.back().second.getBody(),
        currentNestedForOps.back().second.getBody()->begin());
  }

  // Replace induction variable references from those introduced by a
  // single krnl.iterate to those introduced by multiple affine.for
  // operations.
  for (int64_t i = 0; i < (int64_t)currentNestedForOps.size() - 1; i++) {
    auto iterateIV = iterateOp.bodyRegion().front().getArgument(0);
    auto forIV = currentNestedForOps[i].second.getBody()->getArgument(0);
    iterateIV.replaceAllUsesWith(forIV);
    iterateOp.bodyRegion().front().eraseArgument(0);
  }

  // Pop krnl.iterate body region block arguments, leave the last one
  // for convenience (it'll be taken care of by region inlining).
  while (iterateOp.bodyRegion().front().getNumArguments() > 1)
    iterateOp.bodyRegion().front().eraseArgument(0);

  if (currentNestedForOps.empty()) {
    // If no loops are involved, simply move operations from within iterateOp
    // body region to the parent region of iterateOp.
    rewriter.setInsertionPoint(iterateOp);
    iterateOp.bodyRegion().walk([&](Operation *op) {
      if (!op->isKnownTerminator())
        op->replaceAllUsesWith(rewriter.clone(*op));
    });
  } else {
    // Transfer krnl.iterate region to innermost for op.
    auto innermostForOp = currentNestedForOps.back().second;
    innermostForOp.region().getBlocks().clear();
    auto &region = innermostForOp.region();

    region.getBlocks().splice(region.end(), iterateOp.bodyRegion().getBlocks());
    //    rewriter.inlineRegionBefore(iterateOp.bodyRegion(), region,
    //    region.end());
  }

  iterateOp.erase();
  nestedForOps.insert(nestedForOps.end(), currentNestedForOps.begin(),
      currentNestedForOps.end());
}

//===----------------------------------------------------------------------===//
// Krnl to Affine Rewrite Patterns: KrnlTerminator operation.
//===----------------------------------------------------------------------===//

class KrnlTerminatorLowering : public OpRewritePattern<KrnlTerminatorOp> {
public:
  using OpRewritePattern<KrnlTerminatorOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlTerminatorOp op, PatternRewriter &rewriter) const override {
    rewriter.replaceOpWithNewOp<AffineTerminatorOp>(op);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Krnl to Affine Rewrite Patterns: KrnlDefineLoops operation.
//===----------------------------------------------------------------------===//

class KrnlDefineLoopsLowering : public OpRewritePattern<KrnlDefineLoopsOp> {
public:
  using OpRewritePattern<KrnlDefineLoopsOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlDefineLoopsOp op, PatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Krnl to Affine Rewrite Patterns: KrnlOptimizeLoops operation.
//===----------------------------------------------------------------------===//

class KrnlOptimizeLoopsLowering : public OpRewritePattern<KrnlOptimizeLoopsOp> {
public:
  using OpRewritePattern<KrnlOptimizeLoopsOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlOptimizeLoopsOp op, PatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Krnl to Affine Rewrite Patterns: KrnlOptimizeLoops operation.
//===----------------------------------------------------------------------===//

class KrnlBlockLowering : public OpRewritePattern<KrnlBlockOp> {
public:
  using OpRewritePattern<KrnlBlockOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlBlockOp op, PatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// KrnlToAffineLoweringPass
//===----------------------------------------------------------------------===//

/// This is a partial lowering to affine loops of the krnl dialect operations.
/// At this stage the dialect will contain standard operations as well like
/// add and multiply, this pass will leave these operations intact.
namespace {
struct KrnlToAffineLoweringPass
    : public PassWrapper<KrnlToAffineLoweringPass, FunctionPass> {
  void runOnFunction() final;
};
} // end anonymous namespace.

void KrnlToAffineLoweringPass::runOnFunction() {
  auto function = getFunction();

  auto isIterateOpNested = [&](KrnlIterateOp iterateOp) {
    // krnl.iterate is dynamically legal, if and only if it is enclosed by
    // another krnl.iterate.
    Operation *op = iterateOp;
    while ((op = op->getParentOp()))
      if (auto parentOp = dyn_cast<KrnlIterateOp>(op))
        return true;
    return false;
  };

  auto nextIterateOp = [&]() {
    Optional<KrnlIterateOp> nextIterateOp;
    function.walk([&](KrnlIterateOp op) {
      if (!isIterateOpNested(op))
        nextIterateOp = op;
    });
    return nextIterateOp;
  };

  OpBuilder builder(&getContext());
  while (auto iterateOp = nextIterateOp()) {
    auto hasOneChildOp = [](KrnlIterateOp op) {
      auto childrenOps = op.bodyRegion().getOps();
      auto numTerminatorOp = 1;
      return std::distance(childrenOps.begin(), childrenOps.end()) ==
             1 + numTerminatorOp;
    };
    auto childrenContainIterateOp = [](KrnlIterateOp op) {
      auto childrenIterateOps = op.bodyRegion().getOps<KrnlIterateOp>();
      return std::distance(
                 childrenIterateOps.begin(), childrenIterateOps.end()) > 0;
    };

    auto rootOp = iterateOp;
    SmallVector<KrnlIterateOp, 4> perfectlyNestedIterateOps = {*rootOp};
    while (hasOneChildOp(*rootOp) && childrenContainIterateOp(*rootOp)) {
      auto nestedIterateOp =
          *rootOp->bodyRegion().getOps<KrnlIterateOp>().begin();
      perfectlyNestedIterateOps.emplace_back(nestedIterateOp);
      rootOp = nestedIterateOp;
    }

    SmallVector<std::pair<Value, AffineForOp>, 4> nestedForOps;
    for (auto op : perfectlyNestedIterateOps) {
      lowerIterateOp(op, builder, nestedForOps);
    }

    ConversionTarget target(getContext());

    target.addLegalDialect<AffineDialect, StandardOpsDialect>();
    // We expect IR to be free of Krnl Dialect Ops.
    target.addIllegalDialect<KrnlOpsDialect>();

    // Operations that should be converted to LLVM IRs directly.
    target.addLegalOp<KrnlMemcpyOp>();
    target.addLegalOp<KrnlEntryPointOp>();
    target.addLegalOp<KrnlGlobalOp>();
    target.addLegalOp<KrnlGetRefOp>();

    // Operations that pertain to schedules.
    target.addLegalOp<KrnlBlockOp>();
    target.addLegalOp<KrnlDefineLoopsOp>();

    OwningRewritePatternList patterns;
    patterns.insert<KrnlTerminatorLowering, KrnlDefineLoopsLowering,
        KrnlOptimizeLoopsLowering, KrnlBlockLowering>(&getContext());

    if (failed(applyPartialConversion(getFunction(), target, patterns))) {
      signalPassFailure();
    }

    for (auto loopRefToForOp : nestedForOps) {
      Value loopRef;
      AffineForOp forOp;
      std::tie(loopRef, forOp) = loopRefToForOp;
      for (const auto &user : loopRef.getUsers()) {
        if (isa<KrnlBlockOp>(user)) {
          printf("Tiling!\n");
          assert(user->getNumOperands() == 1);
          SmallVector<AffineForOp, 4> tiledLoops;
          SmallVector<AffineForOp, 4> loopsToTile = {forOp};
          if (failed(tilePerfectlyNested(loopsToTile,
                  cast<KrnlBlockOp>(user).tile_sizeAttr().getInt(),
                  &tiledLoops)))
            printf("tiling failed!\n");
        }
      }
    }

    target.addIllegalOp<KrnlDefineLoopsOp>();
    target.addIllegalOp<KrnlBlockOp>();
    if (failed(applyPartialConversion(getFunction(), target, patterns))) {
      signalPassFailure();
    }

    return;
  }
}

} // namespace

std::unique_ptr<Pass> mlir::createLowerKrnlPass() {
  return std::make_unique<KrnlToAffineLoweringPass>();
}

static PassRegistration<KrnlToAffineLoweringPass> pass(
    "lower-krnl", "Lower Krnl dialect.");
