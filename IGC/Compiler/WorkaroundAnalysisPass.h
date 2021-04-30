/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#pragma once

#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CodeGenContextWrapper.hpp"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ADT/SmallSet.h>
#include "common/LLVMWarningsPop.hpp"
#include "common/IGCIRBuilder.h"

namespace IGC
{
    class WorkaroundAnalysis : public llvm::FunctionPass,
        public llvm::InstVisitor<WorkaroundAnalysis>
    {
        llvm::IGCIRBuilder<>* m_builder;
    public:
        static char ID;

        WorkaroundAnalysis();

        ~WorkaroundAnalysis() {}

        virtual bool runOnFunction(llvm::Function& F) override;

        virtual llvm::StringRef getPassName() const override
        {
            return "WorkaroundAnalysis Pass";
        }

        virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override
        {
            AU.addRequired<MetaDataUtilsWrapper>();
            AU.addRequired<CodeGenContextWrapper>();
        }

        void visitCallInst(llvm::CallInst& I);

    private:
        void GatherOffsetWorkaround(llvm::SamplerGatherIntrinsic* gatherpo);
        void ldmsOffsetWorkaournd(llvm::LdMSIntrinsic* ldms);
        const llvm::DataLayout* m_pDataLayout;
        llvm::Module* m_pModule;
        CodeGenContextWrapper* m_pCtxWrapper;
    };

    class WAFMinFMax : public llvm::FunctionPass,
        public llvm::InstVisitor<WAFMinFMax>
    {
    public:
        static char ID;
        WAFMinFMax();
        virtual ~WAFMinFMax() { }

        bool runOnFunction(llvm::Function& F) override;

        llvm::StringRef getPassName() const override
        {
            return "WAFMinFMax";
        }
        void getAnalysisUsage(llvm::AnalysisUsage& AU) const override
        {
            AU.addRequired<MetaDataUtilsWrapper>();
            AU.addRequired<CodeGenContextWrapper>();
        }

        void visitCallInst(llvm::CallInst& I);

    private:
        llvm::IGCIRBuilder<>* m_builder;
        CodeGenContext* m_ctx;
    };

} // namespace IGC
