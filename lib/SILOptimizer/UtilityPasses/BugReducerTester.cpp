//===--- BugReducerTester.cpp ---------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This pass is a testing pass for sil-bug-reducer. It asserts when it visits a
/// function that calls a function specified by an llvm::cl::opt.
///
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

static llvm::cl::opt<std::string> FunctionTarget(
    "bug-reducer-tester-target-func",
    llvm::cl::desc("Function that when called by an apply should cause "
                   "BugReducerTester to blow up or miscompile if the pass "
                   "visits the apply"));

namespace {
enum class FailureKind {
  OptimizerCrasher,
  RuntimeMiscompile,
  RuntimeCrasher,
  None
};
} // end anonymous namespace

static llvm::cl::opt<FailureKind> TargetFailureKind(
    "bug-reducer-tester-failure-kind",
    llvm::cl::desc("The type of failure to perform"),
    llvm::cl::values(
        clEnumValN(FailureKind::OptimizerCrasher, "opt-crasher",
                   "Crash the optimizer when we see the specified apply"),
        clEnumValN(FailureKind::RuntimeMiscompile, "miscompile",
                   "Delete the target function call to cause a runtime "
                   "miscompile that is not a crasher"),
        clEnumValN(FailureKind::RuntimeCrasher, "runtime-crasher",
                   "Delete the target function call to cause a runtime "
                   "miscompile that is not a crasher"),
        clEnumValEnd),
    llvm::cl::init(FailureKind::None));

namespace {

class BugReducerTester : public SILFunctionTransform {

  // We only want to cause 1 miscompile.
  bool CausedError = false;
  StringRef RuntimeCrasherFunctionName = "bug_reducer_runtime_crasher_func";

  SILFunction *getRuntimeCrasherFunction() {
    assert(TargetFailureKind == FailureKind::RuntimeCrasher);
    llvm::SmallVector<SILResultInfo, 1> ResultInfoArray;
    auto EmptyTupleCanType = getFunction()
                                 ->getModule()
                                 .Types.getEmptyTupleType()
                                 .getSwiftRValueType();
    ResultInfoArray.push_back(
        SILResultInfo(EmptyTupleCanType, ResultConvention::Unowned));
    auto FuncType = SILFunctionType::get(
        nullptr, SILFunctionType::ExtInfo(SILFunctionType::Representation::Thin,
                                          false /*isPseudoGeneric*/),
        ParameterConvention::Direct_Unowned, ArrayRef<SILParameterInfo>(),
        ResultInfoArray, None, getFunction()->getModule().getASTContext());

    SILFunction *F = getFunction()->getModule().getOrCreateSharedFunction(
        RegularLocation::getAutoGeneratedLocation(), RuntimeCrasherFunctionName,
        FuncType, IsBare, IsNotTransparent, IsFragile, IsNotThunk);
    if (F->isDefinition())
      return F;

    // Create a new block.
    SILBasicBlock *BB = F->createBasicBlock();

    // Insert a builtin int trap. Then return F.
    SILBuilder B(BB);
    B.createBuiltinTrap(RegularLocation::getAutoGeneratedLocation());
    B.createUnreachable(ArtificialUnreachableLocation());
    return F;
  }

  void run() override {
    // If we don't have a target function or we already caused a miscompile,
    // just return.
    if (FunctionTarget.empty() || CausedError)
      return;
    assert(TargetFailureKind != FailureKind::None);
    SILModule &M = getFunction()->getModule();
    for (auto &BB : *getFunction()) {
      for (auto &II : BB) {
        auto *Apply = dyn_cast<ApplyInst>(&II);
        if (!Apply)
          continue;
        auto *FRI = dyn_cast<FunctionRefInst>(Apply->getCallee());
        if (!FRI ||
            !FRI->getReferencedFunction()->getName().equals(FunctionTarget))
          continue;

        // Ok, we found the Apply that we want! If we are asked to crash, crash
        // here.
        if (TargetFailureKind == FailureKind::OptimizerCrasher)
          llvm_unreachable("Found the target!");

        // Otherwise, if we are asked to perform a runtime time miscompile,
        // delete the apply target.
        if (TargetFailureKind == FailureKind::RuntimeMiscompile) {
          Apply->replaceAllUsesWith(SILUndef::get(Apply->getType(), M));
          Apply->eraseFromParent();

          // Mark that we found the miscompile and return so we do not try to
          // visit any more instructions in this function.
          CausedError = true;
          return;
        }

        assert(TargetFailureKind == FailureKind::RuntimeCrasher);
        // Finally, if we reach this point we are being asked to replace the
        // given apply with a new apply that calls the crasher func.
        auto Loc = RegularLocation::getAutoGeneratedLocation();
        SILFunction *RuntimeCrasherFunc = getRuntimeCrasherFunction();
        llvm::dbgs() << "Runtime Crasher Func!\n";
        RuntimeCrasherFunc->dump();
        SILBuilder B(Apply->getIterator());
        B.createApply(Loc, B.createFunctionRef(Loc, RuntimeCrasherFunc),
                      RuntimeCrasherFunc->getLoweredType(),
                      M.Types.getEmptyTupleType(), ArrayRef<Substitution>(),
                      ArrayRef<SILValue>(), false /*NoThrow*/);

        Apply->replaceAllUsesWith(SILUndef::get(Apply->getType(), M));
        Apply->eraseFromParent();

        CausedError = true;
        return;
      }
    }
  }

  StringRef getName() override { return "Bug Reducer Tester"; }
};

} // end anonymous namespace

SILTransform *swift::createBugReducerTester() { return new BugReducerTester(); }