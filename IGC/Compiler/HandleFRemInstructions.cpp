/*========================== begin_copyright_notice ============================

Copyright (C) 2019-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Compiler/HandleFRemInstructions.hpp"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include "common/LLVMWarningsPop.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Probe/Assertion.h"

using namespace llvm;
using namespace IGC;


#define PASS_FLAG "igc-handle-frem-inst"
#define PASS_DESCRIPTION "Replace FRem instructions with proper builtin calls"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(HandleFRemInstructions, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(HandleFRemInstructions, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)


char HandleFRemInstructions::ID = 0;

HandleFRemInstructions::HandleFRemInstructions() : ModulePass(ID)
{
    initializeHandleFRemInstructionsPass(*PassRegistry::getPassRegistry());
}

void HandleFRemInstructions::visitFRem(llvm::BinaryOperator& I)
{
    auto Val1 = I.getOperand(0);
    auto Val2 = I.getOperand(1);
    auto ValType = Val1->getType();
    auto ScalarType = ValType->getScalarType();
    SmallVector<Type*, 2> ArgsTypes{ ValType, ValType };

    IGC_ASSERT_MESSAGE(Val1->getType() == Val2->getType(), "Operands of frem instruction must have same type");
    IGC_ASSERT_MESSAGE(ScalarType->isFloatingPointTy(), "Operands of frem instruction must have floating point type");

    std::string VecStr = "";
    std::string FpTypeStr;

    if (ScalarType->isHalfTy() || ScalarType->isFloatTy() || ScalarType->isDoubleTy())
    {
        auto TypeWidth = ScalarType->getScalarSizeInBits();
        FpTypeStr = "f" + std::to_string(TypeWidth);
    }
    else
    {
        IGC_ASSERT_MESSAGE(0, "Unsupported type");
    }

    if (ValType->isVectorTy())
    {
        auto VecCount = cast<VectorType>(ValType)->getNumElements();
        if (VecCount == 2 || VecCount == 3 || VecCount == 4 || VecCount == 8 || VecCount == 16)
        {
            VecStr = "v" + std::to_string(VecCount);
        }
        else
        {
            IGC_ASSERT_MESSAGE(0, "Unsupported vector size");
        }
    }
    std::string TypeStr = "_" + VecStr + FpTypeStr;
    std::string FuncName = "__builtin_spirv_OpFRem" + TypeStr + TypeStr;
    auto FT = FunctionType::get(Val1->getType(), ArgsTypes, false);
    auto Callee = m_module->getOrInsertFunction(FuncName, FT);
    SmallVector<Value*, 2> FuncArgs{ Val1, Val2 };
    auto Call = CallInst::Create(Callee, FuncArgs, "");
    ReplaceInstWithInst(&I, Call);
    m_changed = true;
}

bool HandleFRemInstructions::runOnModule(llvm::Module& M)
{
    m_changed = false;
    m_module = &M;

    visit(M);

    m_module = nullptr;
    return m_changed;
}
