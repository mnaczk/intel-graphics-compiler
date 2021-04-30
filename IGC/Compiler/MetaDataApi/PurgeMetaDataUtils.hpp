/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#pragma once

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include "common/LLVMWarningsPop.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CodeGenContextWrapper.hpp"

namespace IGC
{
    class MetaDataUtilsWrapper;
    class PurgeMetaDataUtils : public llvm::ModulePass
    {
    public:
        PurgeMetaDataUtils();

        ~PurgeMetaDataUtils() {}

        virtual llvm::StringRef getPassName() const override
        {
            return "PurgeMetaDataUtilsPass";
        }

        virtual bool runOnModule(llvm::Module& M) override;

        virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override {
            AU.addRequired<MetaDataUtilsWrapper>();
            AU.addRequired<CodeGenContextWrapper>();
        }

        // Pass identification, replacement for typeid
        static char ID;
    protected:
        llvm::Module* m_pModule;

        /// @brief  Indicates if the pass changed the processed function
        bool m_changed;
    };

} // namespace IGC
