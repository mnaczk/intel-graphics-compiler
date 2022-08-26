/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include "VLD.hpp"
#include "Probe/Assertion.h"
#include "VLD_SPIRVSplitter.hpp"
#include "ocl_igc_interface/impl/igc_ocl_translation_ctx_impl.h"
#include "spirv/unified1/spirv.hpp"
#include <ZEInfoYAML.hpp>

#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/YAMLTraits.h>

#include <algorithm>
#if (defined(__GNUC__) && __GNUC__ >= 9) || (defined(_MSC_VER) && (_MSVC_LANG >= 201703L))
// Temporary WA for VC issue.
#include <filesystem>
#include <fstream>
#endif

namespace TC {
    // Declarations for utility functions declared in other libraries that will be linked.
    void DumpShaderFile(const std::string &dstDir, const char *pBuffer,
        const UINT bufferSize, const QWORD hash,
        const std::string &ext, std::string *fileName = nullptr);
    spv_result_t DisassembleSPIRV(const char* pBuffer, UINT bufferSize,
        spv_text* outSpirvAsm);
}

namespace {
  static const std::string ERROR_VLD = "VLD: Failed to compile SPIR-V with following error: \n";

  llvm::Expected<std::vector<llvm::StringRef>> getZeBinSectionsData(llvm::StringRef ZeBinary, zebin::SHT_ZEBIN SectionType) {
    using namespace llvm;
    MemoryBufferRef inputRef(ZeBinary, "zebin");
    std::vector<StringRef> OutVec;

    auto ElfOrErr = object::ObjectFile::createELFObjectFile(inputRef);
    if (!ElfOrErr)
      return ElfOrErr.takeError();
#if LLVM_VERSION_MAJOR < 12
    auto ElfFilePointer = cast<object::ELF64LEObjectFile>(*ElfOrErr.get()).getELFFile();
    IGC_ASSERT(ElfFilePointer);
    auto ElfFile = *ElfFilePointer;
#else
    auto ElfFile = cast<object::ELF64LEObjectFile>(*ElfOrErr.get()).getELFFile();
#endif
    auto ElfSections = ElfFile.sections();
    if (!ElfSections)
      return ElfSections.takeError();

    for (auto &Sect : *ElfSections) {
      if (Sect.sh_type == SectionType) {
#if LLVM_VERSION_MAJOR < 12
        auto SectionDataOrErr = ElfFile.getSectionContents(&Sect);
#else
        auto SectionDataOrErr = ElfFile.getSectionContents(Sect);
#endif
        if (!SectionDataOrErr)
          return SectionDataOrErr.takeError();
        StringRef Data(reinterpret_cast<const char *>((*SectionDataOrErr).data()),
          (size_t)Sect.sh_size);
        OutVec.push_back(Data);
      }
    }

    return OutVec;
  }

// Extracts .visaasm sections from input zeBinary ELF.
// Returns a vector of strings - one for each section.
llvm::Expected<std::vector<llvm::StringRef>>
GetVISAAsmFromZEBinary(llvm::StringRef ZeBinary) {
  return getZeBinSectionsData(ZeBinary, zebin::SHT_ZEBIN_VISAASM);
}

llvm::Expected<int> GetSIMDSizeFromZeBinary(llvm::StringRef ZeBinary) {
  using namespace llvm;
  auto ZeInfoYAMLOrErr = getZeBinSectionsData(ZeBinary, zebin::SHT_ZEBIN_ZEINFO);
  if (!ZeInfoYAMLOrErr) {
    return ZeInfoYAMLOrErr.takeError();
  }
  if (ZeInfoYAMLOrErr->size() != 1) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "ZEBinary expected to contain exactly one .ze_info section!");
  }

  // ZeBinary strings are not null-terminated, so copy it to std::string.
  std::string ZeInfoYAML = (*(ZeInfoYAMLOrErr->begin())).str();

  llvm::yaml::Input yin(ZeInfoYAML.c_str());
  zebin::zeInfoContainer ZeInfo;
  yin >> ZeInfo;
  if (yin.error()) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "Failed to parse .ze_info section!");
  }

  std::vector<int> SimdSizes;
  for (auto& Kernel : ZeInfo.kernels) {
    SimdSizes.push_back(Kernel.execution_env.simd_size);
  }
  for (auto& Func : ZeInfo.functions) {
    SimdSizes.push_back(Func.execution_env.simd_size);
  }
  if (SimdSizes.size() == 0) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "Couldn't find any compiled kernel or function SIMD size!");
  }
  if (!std::all_of(SimdSizes.begin(), SimdSizes.end(), [&](auto& SimdSize) { return SimdSize == *SimdSizes.begin(); })) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "SIMD sizes in the module are not uniform!");
  }

  return *SimdSizes.begin();
}

void DumpSPIRVFile(const char *programData, size_t programSizeInBytes,
                   const ShaderHash &inputShHash, std::string ext) {
  const char *pOutputFolder = IGC::Debug::GetShaderOutputFolder();

  TC::DumpShaderFile(pOutputFolder, programData, programSizeInBytes,
                     inputShHash.getAsmHash(), ext);
  spv_text spirvAsm = nullptr;
  if (TC::DisassembleSPIRV(programData, programSizeInBytes, &spirvAsm) ==
      SPV_SUCCESS) {
    TC::DumpShaderFile(pOutputFolder, spirvAsm->str, spirvAsm->length,
                       inputShHash.getAsmHash(), ext + "asm");
  }
  spvTextDestroy(spirvAsm);
}

} // namespace

namespace IGC {
namespace VLD {
using namespace TC;

// Translates ESIMD and SPMD code in the module.
// 3 cases are handled:
// 1. only SPMD code is present
// 2. only ESIMD code is present
// 3. ESIMD code is invoked from SPMD code
//
// The general flow is:
// 1. Split input SPIR-V module into SPMD and ESIMD parts
// 2. Invoke SPMD and ESIMD backends with appropriate SPIR-V modules
// 3. If SPMD code invokes ESIMD code, extract .visaasm from the each output zeBinary
// TODO: 4. Link .visaasm files via vISA interfaces
//
// The function signature corresponds to TC::TranslateBuild interface, so that
// it is easy to pass same arguments to SPMD and VC backends.
//
// Assumptions:
// 1. ZEBinary output format is used in SPMD+ESIMD case.
// TODO: error out if patch token output format is used.
bool TranslateBuildSPMDAndESIMD(const TC::STB_TranslateInputArgs *pInputArgs,
                                  TC::STB_TranslateOutputArgs *pOutputArgs,
                                  TC::TB_DATA_FORMAT inputDataFormatTemp,
                                  const IGC::CPlatform &IGCPlatform,
                                  float profilingTimerResolution,
                                  const ShaderHash &inputShHash,
                                  std::string& errorMessage) {

  IGC_ASSERT(inputDataFormatTemp == TB_DATA_FORMAT_SPIR_V);

    // Split ESIMD and SPMD code.
  auto spmd_esimd_programs_or_err = VLD::SplitSPMDAndESIMD(
      pInputArgs->pInput, pInputArgs->InputSize);

  if (!spmd_esimd_programs_or_err) {
      // The error must be handled. Doing nothing for now.
      handleAllErrors(spmd_esimd_programs_or_err.takeError(),
                      [](const llvm::ErrorInfoBase &EI) {});
      // Workaround: try to compile on SPMD path if splitting failed.
      // This is because not all VC opcodes are merged to SPIR-V Tools.
      return TranslateBuildSPMD(pInputArgs, pOutputArgs, inputDataFormatTemp,
          IGCPlatform, profilingTimerResolution,
          inputShHash);

      // TODO: uncomment once above workaround is removed.
      // Caller releases the error string, so we need to make a copy of the error message here.
      // TODO: pOutputArgs contains field for error string so we can copy it there.
      // Not done now, as it would require copy-paste code that is avaiable in dllinterfacecompute. Needs to be refactored.
      // errorMessage = llvm::toString(spmd_esimd_programs_or_err.takeError());
      // return false;
  }

  std::string newOptions{pInputArgs->pOptions ? pInputArgs->pOptions : ""};
  std::string esimdOptions{ newOptions };
  esimdOptions += " -vc-codegen";

  auto [spmdProg, esimdProg] = spmd_esimd_programs_or_err.get();

  IGC_ASSERT(!spmdProg.empty() || !esimdProg.empty());
  if (spmdProg.empty()) {
#if defined(IGC_VC_ENABLED)
      // Only ESIMD code detected.
      STB_TranslateInputArgs newArgs = *pInputArgs;
      newArgs.pOptions = esimdOptions.data();
      newArgs.OptionsSize = esimdOptions.size();
      return TranslateBuildVC(&newArgs, pOutputArgs, inputDataFormatTemp,
                              IGCPlatform, profilingTimerResolution,
                              inputShHash);
#else // defined(IGC_VC_ENABLED)
      errorMessage = "ESIMD code detected, but VC not enabled in this build.";
      return false;
#endif // defined(IGC_VC_ENABLED)
  } else if (esimdProg.empty()) {
      // Only SPMD code detected.
      return TranslateBuildSPMD(pInputArgs, pOutputArgs, inputDataFormatTemp,
          IGCPlatform, profilingTimerResolution,
          inputShHash);
  }

  // SPMD+ESIMD code detected.

  if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable)) {
      DumpSPIRVFile(pInputArgs->pInput, pInputArgs->InputSize, inputShHash, ".spmd_and_esimd.spv");
      DumpSPIRVFile((const char*)spmdProg.data(), spmdProg.size() * sizeof(uint32_t), inputShHash, ".spmd_split.spv");
      DumpSPIRVFile((const char*)esimdProg.data(), esimdProg.size() * sizeof(uint32_t), inputShHash, ".esimd_split.spv");
  }

  STB_TranslateInputArgs newArgsSPMD = *pInputArgs;
  newArgsSPMD.pInput = reinterpret_cast<char *>(spmdProg.data());
  newArgsSPMD.InputSize = spmdProg.size() * sizeof(*spmdProg.begin());

  STB_TranslateInputArgs newArgsESIMD = *pInputArgs;
  newArgsESIMD.pInput = reinterpret_cast<char *>(esimdProg.data());
  newArgsESIMD.InputSize = esimdProg.size() * sizeof(*esimdProg.begin());
  newArgsESIMD.pOptions = esimdOptions.data();
  newArgsESIMD.OptionsSize = esimdOptions.size();

  std::array<SPVTranslationPair, 2> SpvArr{
      std::make_pair(SPIRVTypeEnum::SPIRV_ESIMD, newArgsESIMD),
      std::make_pair(SPIRVTypeEnum::SPIRV_SPMD, newArgsSPMD)
  };

  return TranslateBuildSPMDAndESIMD(
      SpvArr, pOutputArgs, inputDataFormatTemp,
      IGCPlatform, profilingTimerResolution, inputShHash, errorMessage);
}

bool TranslateBuildSPMDAndESIMD(
    llvm::ArrayRef<SPVTranslationPair> InputModules,
    TC::STB_TranslateOutputArgs *pOutputArgs,
    TC::TB_DATA_FORMAT inputDataFormatTemp, const IGC::CPlatform &IGCPlatform,
    float profilingTimerResolution, const ShaderHash &inputShHash,
    std::string &errorMessage) {
#if defined(IGC_VC_ENABLED)

  std::vector<std::string> visaStrings;
  std::vector<const char*> visaCStrings;
  int SimdSize = 0;

  for (auto& InputArgsPair : InputModules) {
    auto& InputArgs = InputArgsPair.second;

    TC::STB_TranslateOutputArgs NewOutputArgs;
    CIF::SafeZeroOut(NewOutputArgs);
    auto outputData = std::unique_ptr<char[]>(NewOutputArgs.pOutput);
    auto errorString = std::unique_ptr<char[]>(NewOutputArgs.pErrorString);
    auto debugData = std::unique_ptr<char[]>(NewOutputArgs.pDebugData);

    STB_TranslateInputArgs NewInputArgs = InputArgs;
    std::string NewInternalOptions{InputArgs.pInternalOptions
      ? InputArgs.pInternalOptions
      : ""};

    switch (InputArgsPair.first) {
    case VLD::SPIRVTypeEnum::SPIRV_SPMD:
      NewInternalOptions += " -ze-emit-zebin-visa-sections";
      break;
    case VLD::SPIRVTypeEnum::SPIRV_ESIMD:
      NewInternalOptions += " -emit-zebin-visa-sections";
      NewInternalOptions += " -binary-format=ze";
      if (SimdSize != 0) {
        NewInternalOptions += " -vc-interop-subgroup-size ";
        NewInternalOptions += std::to_string(SimdSize);
      }
      break;
    default:
      errorMessage = "Unsupported SPIR-V flavour detected!";
      return false;
    }

    NewInputArgs.pVISAAsmToLinkArray = (!visaCStrings.empty()) ? visaCStrings.data() : nullptr;
    NewInputArgs.NumVISAAsmsToLink = visaCStrings.size();
    NewInputArgs.pInternalOptions = NewInternalOptions.data();
    NewInputArgs.InternalOptionsSize = NewInternalOptions.size();
    bool success = false;
    if (InputArgsPair.first == VLD::SPIRVTypeEnum::SPIRV_SPMD) {
      success = TranslateBuildSPMD(&NewInputArgs, &NewOutputArgs,
                                   inputDataFormatTemp, IGCPlatform,
                                   profilingTimerResolution, inputShHash);
    } else {
      IGC_ASSERT(InputArgsPair.first == VLD::SPIRVTypeEnum::SPIRV_ESIMD);
      success =
          TranslateBuildVC(&NewInputArgs, &NewOutputArgs, inputDataFormatTemp,
                           IGCPlatform, profilingTimerResolution, inputShHash);
    }

    if (!success) {
      errorMessage = "VLD: Failed to compile SPIR-V with following error: \n";
      errorMessage += NewOutputArgs.pErrorString;
      return false;
    }

    // If this is the last SPIR-V to compile, stop here. The rest of the code handles
    // extracting information for further compilations.
    if(&InputArgsPair == &InputModules.back()) {
      *pOutputArgs = NewOutputArgs;
      break;
    }

    llvm::StringRef ZeBinary(NewOutputArgs.pOutput, NewOutputArgs.OutputSize);

    // Set SimdSize based on first SPMD module, as ESIMD always returns 1.
    if (InputArgsPair.first == SPIRVTypeEnum::SPIRV_SPMD) {
      auto SimdSizeOrErr = GetSIMDSizeFromZeBinary(ZeBinary);
      if (!SimdSizeOrErr) {
        errorMessage = ERROR_VLD + llvm::toString(SimdSizeOrErr.takeError());
        return false;
      }
      if (SimdSize != 0 && SimdSize != *SimdSizeOrErr) {
        errorMessage =
            ERROR_VLD +
            "Compilation of SPIR-V modules resulted in different SIMD sizes!";
        return false;
      }
      if (SimdSize == 0)
        SimdSize = *SimdSizeOrErr;
    }

    auto VISAAsm =
      GetVISAAsmFromZEBinary(ZeBinary);

    if (!VISAAsm) {
      errorMessage =
        "VLD: Failed to compile SPIR-V with following error: \n" +
        llvm::toString(VISAAsm.takeError());
      return false;
    }

    if (VISAAsm->empty()) {
      errorMessage = "VLD: ZeBinary did not contain any .visaasm sections!";
      return false;
    }

    // ZeBinary contains non-null terminated strings, add the null via std::string ownership.
    for (auto& s : *VISAAsm) {
      visaStrings.push_back(s.str());
      visaCStrings.push_back(visaStrings.back().c_str());
    }
  }

  return true;

#else // defined(IGC_VC_ENABLED)
    errorMessage = "Could not compile ESIMD part of SPIR-V module, as VC is not included in this build.";
    return false;
#endif // defined(IGC_VC_ENABLED)
}
} // namespace VLD
} // namespace IGC
