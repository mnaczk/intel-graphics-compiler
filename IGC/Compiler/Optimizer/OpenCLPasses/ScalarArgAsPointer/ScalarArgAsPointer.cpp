/*========================== begin_copyright_notice ============================

Copyright (C) 2022 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "Compiler/Optimizer/OpenCLPasses/ScalarArgAsPointer/ScalarArgAsPointer.hpp"
#include "Compiler/CISACodeGen/OpenCLKernelCodeGen.hpp"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include "common/LLVMWarningsPop.hpp"

#include "Probe/Assertion.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

// Register pass to igc-opt
#define PASS_FLAG "igc-scalar-arg-as-pointer-analysis"
#define PASS_DESCRIPTION "Analyzes scalar kernel arguments used for global memory access"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ScalarArgAsPointerAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(ScalarArgAsPointerAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char ScalarArgAsPointerAnalysis::ID = 0;

ScalarArgAsPointerAnalysis::ScalarArgAsPointerAnalysis() : ModulePass(ID)
{
    initializeScalarArgAsPointerAnalysisPass(*PassRegistry::getPassRegistry());
}

bool ScalarArgAsPointerAnalysis::runOnModule(Module& M)
{
    DL = &M.getDataLayout();

    MetaDataUtils* MDUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();

    bool changed = false;

    for (Function& F : M)
    {
        if (F.isDeclaration())
            continue;

        if (!isEntryFunc(MDUtils, &F))
            continue;

        changed |= analyzeFunction(F);
    }

    // Update LLVM metadata based on IGC MetadataUtils
    if (changed)
        MDUtils->save(M.getContext());

    return changed;
}

bool ScalarArgAsPointerAnalysis::analyzeFunction(llvm::Function& F)
{
    m_matchingArgs.clear();
    m_visitedInst.clear();
    m_allocas.clear();

    visit(F);

    if (m_matchingArgs.empty())
        return false;

    FunctionMetaData& funcMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData()->FuncMD[&F];

    for (auto it = m_matchingArgs.begin(); it != m_matchingArgs.end(); ++it)
        funcMD.m_OpenCLArgScalarAsPointers.insert((*it)->getArgNo());

    return true;
}

void ScalarArgAsPointerAnalysis::visitStoreInst(llvm::StoreInst& I)
{
    analyzeStoredArg(I);
    analyzePointer(I.getPointerOperand());
}

void ScalarArgAsPointerAnalysis::visitLoadInst(llvm::LoadInst& I)
{
    analyzePointer(I.getPointerOperand());
}

void ScalarArgAsPointerAnalysis::analyzePointer(llvm::Value* V)
{
    auto* type = dyn_cast<PointerType>(V->getType());

    IGC_ASSERT_MESSAGE(type, "Value should be a pointer");

    if (type->getAddressSpace() != ADDRESS_SPACE_GLOBAL)
        return;

    // If scalar is going to be used as pointer, it has to be an instruction, like casting.
    auto* inst = dyn_cast<Instruction>(V);
    if (!inst)
        return;

    if (const ArgSet* args = findArgs(inst))
        m_matchingArgs.insert(args->begin(), args->end());
}

const ScalarArgAsPointerAnalysis::ArgSet* ScalarArgAsPointerAnalysis::findArgs(llvm::Instruction* inst)
{
    // Skip already visited instruction
    if (m_visitedInst.count(inst))
        return &(*m_visitedInst[inst]);

    // Mark as visited
    m_visitedInst.try_emplace(inst, nullptr);

    // Assume intrinsic are safe simple arithmetics.
    if (isa<CallInst>(inst) && !isa<GenIntrinsicInst>(inst))
        return nullptr;

    auto result = std::make_unique<ScalarArgAsPointerAnalysis::ArgSet>();

    if (LoadInst* LI = dyn_cast<LoadInst>(inst))
    {
        if (!findStoredArgs(*LI, *result))
            return nullptr; // (1) Found indirect access, fail search
    }
    else
    {
        // For any other type of instruction trace back operands.
        for (unsigned int i = 0; i < inst->getNumOperands(); ++i)
        {
            Value* op = inst->getOperand(i);

            if (Argument* arg = dyn_cast<Argument>(op))
            {
                // Consider only integer arguments
                if (arg->getType()->isIntegerTy())
                {
                    result->insert(arg);
                }
                else
                {
                    // (2) Found non-compatible argument, fail
                    return nullptr;
                }
            }
            else if (Instruction* opInst = dyn_cast<Instruction>(op))
            {
                auto* args = findArgs(opInst);

                if (!args)
                    return nullptr; // propagate fail

                result->insert(args->begin(), args->end());
            }
        }
    }

    m_visitedInst[inst] = std::move(result);
    return &(*m_visitedInst[inst]);
}

void ScalarArgAsPointerAnalysis::analyzeStoredArg(llvm::StoreInst& SI)
{
    // Only track stores of kernel arguments.
    Argument* A = dyn_cast<Argument>(SI.getValueOperand());
    if (!A)
        return;

    AllocaInst* AI = nullptr;
    GetElementPtrInst* GEPI = nullptr;
    if (!findAllocaWithOffset(SI.getPointerOperand(), AI, GEPI))
        return;

    uint64_t totalOffset = 0;

    if (GEPI)
    {
        // For store instruction offset must be constant.
        APInt offset(DL->getIndexTypeSizeInBits(GEPI->getType()), 0);
        if (!GEPI->accumulateConstantOffset(*DL, offset) || offset.isNegative())
            return;
        totalOffset += offset.getZExtValue();
    }

    m_allocas[std::pair<llvm::AllocaInst*, uint64_t>(AI, totalOffset)] = A;
}

bool ScalarArgAsPointerAnalysis::findStoredArgs(llvm::LoadInst& LI, ArgSet& args)
{
    AllocaInst* AI = nullptr;
    GetElementPtrInst* GEPI = nullptr;
    if (!findAllocaWithOffset(LI.getPointerOperand(), AI, GEPI))
        return false;

    // It is possible one or more GEP operand is a variable index to array type.
    // In this case search for all possible offsets to alloca.
    using Offsets = SmallVector<uint64_t, 4>;
    Offsets offsets;
    offsets.push_back(0);

    if (GEPI)
    {
        for (gep_type_iterator GTI = gep_type_begin(GEPI), prevGTI = gep_type_end(GEPI); GTI != gep_type_end(GEPI); prevGTI = GTI++)
        {
            if (ConstantInt* C = dyn_cast<ConstantInt>(GTI.getOperand()))
            {
                if (C->isZero())
                    continue;

                uint64_t offset = 0;

                if (StructType* STy = GTI.getStructTypeOrNull())
                    offset = DL->getStructLayout(STy)->getElementOffset(int_cast<unsigned>(C->getZExtValue()));
                else
                    offset = C->getZExtValue() * DL->getTypeAllocSize(GTI.getIndexedType()); // array or vector

                for (auto it = offsets.begin(); it != offsets.end(); ++it)
                    *it += offset;
            }
            else
            {
                if (prevGTI == gep_type_end(GEPI))
                    return false; // variable index at first operand, should not happen

                // gep_type_iterator is used to query indexed type. For arrays this is type
                // of single element. To get array size, we need to do query for it at
                // previous iterator step (before stepping into type indexed by array).
                ArrayType* ATy = dyn_cast<ArrayType>(prevGTI.getIndexedType());
                if (!ATy)
                    return false;

                uint64_t arrayElements = ATy->getNumElements();

                uint64_t byteSize = DL->getTypeAllocSize(GTI.getIndexedType());

                Offsets tmp;
                for (auto i = 0; i < arrayElements; ++i)
                    for (auto it = offsets.begin(); it != offsets.end(); ++it)
                        tmp.push_back(*it + i * byteSize);

                offsets = tmp;
            }
        }
    }

    for (auto it = offsets.begin(); it != offsets.end(); ++it)
    {
        std::pair<llvm::AllocaInst*, uint64_t> key(AI, *it);
        if (m_allocas.count(key))
            args.insert(m_allocas[key]);
    }

    return !args.empty();
}

bool ScalarArgAsPointerAnalysis::findAllocaWithOffset(llvm::Value* V, llvm::AllocaInst*& outAI, llvm::GetElementPtrInst*& outGEPI)
{
    IGC_ASSERT_MESSAGE(dyn_cast<PointerType>(V->getType()), "Value should be a pointer");

    outGEPI = nullptr;
    Value* tmp = V;

    while (true)
    {
        if (BitCastInst* BCI = dyn_cast<BitCastInst>(tmp))
        {
            tmp = BCI->getOperand(0);
        }
        else if (GetElementPtrInst* GEPI = dyn_cast<GetElementPtrInst>(tmp))
        {
            if (outGEPI)
                return false; // only one GEP instruction is supported
            outGEPI = GEPI;
            tmp = GEPI->getPointerOperand();
        }
        else if (AllocaInst* AI = dyn_cast<AllocaInst>(tmp))
        {
            outAI = AI;
            return true;
        }
        else
        {
            return false;
        }
    }
}
