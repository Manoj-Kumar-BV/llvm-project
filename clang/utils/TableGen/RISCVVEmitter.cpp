//===-- RISCVVEmitter.cpp - Generate riscv_vector.h for use with clang ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend is responsible for emitting riscv_vector.h which
// includes a declaration and definition of each intrinsic functions specified
// in https://github.com/riscv/rvv-intrinsic-doc.
//
// See also the documentation in include/clang/Basic/riscv_vector.td.
//
//===----------------------------------------------------------------------===//

#include "clang/Support/RISCVVIntrinsicUtils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/StringToOffsetTable.h"
#include <optional>

using namespace llvm;
using namespace clang::RISCV;

namespace {
struct SemaRecord {
  // Intrinsic name, e.g. vadd_vv
  std::string Name;

  // Overloaded intrinsic name, could be empty if can be computed from Name
  // e.g. vadd
  std::string OverloadedName;

  // Supported type, mask of BasicType.
  unsigned TypeRangeMask;

  // Supported LMUL.
  unsigned Log2LMULMask;

  // Required extensions for this intrinsic.
  std::string RequiredExtensions;

  // Prototype for this intrinsic.
  SmallVector<PrototypeDescriptor> Prototype;

  // Suffix of intrinsic name.
  SmallVector<PrototypeDescriptor> Suffix;

  // Suffix of overloaded intrinsic name.
  SmallVector<PrototypeDescriptor> OverloadedSuffix;

  // Number of field, large than 1 if it's segment load/store.
  unsigned NF;

  bool HasMasked :1;
  bool HasVL :1;
  bool HasMaskedOffOperand :1;
  bool HasTailPolicy : 1;
  bool HasMaskPolicy : 1;
  bool HasFRMRoundModeOp : 1;
  bool IsTuple : 1;
  LLVM_PREFERRED_TYPE(PolicyScheme)
  uint8_t UnMaskedPolicyScheme : 2;
  LLVM_PREFERRED_TYPE(PolicyScheme)
  uint8_t MaskedPolicyScheme : 2;
};

// Compressed function signature table.
class SemaSignatureTable {
private:
  std::vector<PrototypeDescriptor> SignatureTable;

  void insert(ArrayRef<PrototypeDescriptor> Signature);

public:
  static constexpr unsigned INVALID_INDEX = ~0U;

  // Create compressed signature table from SemaRecords.
  void init(ArrayRef<SemaRecord> SemaRecords);

  // Query the Signature, return INVALID_INDEX if not found.
  unsigned getIndex(ArrayRef<PrototypeDescriptor> Signature);

  /// Print signature table in RVVHeader Record to \p OS
  void print(raw_ostream &OS);
};

class RVVEmitter {
private:
  const RecordKeeper &Records;
  RVVTypeCache TypeCache;

public:
  RVVEmitter(const RecordKeeper &R) : Records(R) {}

  /// Emit riscv_vector.h
  void createHeader(raw_ostream &o);

  /// Emit all the __builtin prototypes and code needed by Sema.
  void createBuiltins(raw_ostream &o);

  /// Emit all the information needed to map builtin -> LLVM IR intrinsic.
  void createCodeGen(raw_ostream &o);

  /// Emit all the information needed by SemaRISCVVectorLookup.cpp.
  /// We've large number of intrinsic function for RVV, creating a customized
  /// could speed up the compilation time.
  void createSema(raw_ostream &o);

private:
  /// Create all intrinsics and add them to \p Out and SemaRecords.
  void createRVVIntrinsics(std::vector<std::unique_ptr<RVVIntrinsic>> &Out,
                           std::vector<SemaRecord> *SemaRecords = nullptr);
  /// Create all intrinsic records and SemaSignatureTable from SemaRecords.
  void createRVVIntrinsicRecords(std::vector<RVVIntrinsicRecord> &Out,
                                 SemaSignatureTable &SST,
                                 ArrayRef<SemaRecord> SemaRecords);

  /// Print HeaderCode in RVVHeader Record to \p Out
  void printHeaderCode(raw_ostream &OS);
};

} // namespace

static BasicType ParseBasicType(char c) {
  switch (c) {
  case 'c':
    return BasicType::Int8;
    break;
  case 's':
    return BasicType::Int16;
    break;
  case 'i':
    return BasicType::Int32;
    break;
  case 'l':
    return BasicType::Int64;
    break;
  case 'x':
    return BasicType::Float16;
    break;
  case 'f':
    return BasicType::Float32;
    break;
  case 'd':
    return BasicType::Float64;
    break;
  case 'y':
    return BasicType::BFloat16;
    break;
  default:
    return BasicType::Unknown;
  }
}

static VectorTypeModifier getTupleVTM(unsigned NF) {
  assert(2 <= NF && NF <= 8 && "2 <= NF <= 8");
  return static_cast<VectorTypeModifier>(
      static_cast<uint8_t>(VectorTypeModifier::Tuple2) + (NF - 2));
}

static unsigned getIndexedLoadStorePtrIdx(const RVVIntrinsic *RVVI) {
  // We need a special rule for segment load/store since the data width is not
  // encoded in the intrinsic name itself.
  const StringRef IRName = RVVI->getIRName();
  constexpr unsigned RVV_VTA = 0x1;
  constexpr unsigned RVV_VMA = 0x2;

  if (IRName.starts_with("vloxseg") || IRName.starts_with("vluxseg")) {
    bool NoPassthru =
        (RVVI->isMasked() && (RVVI->getPolicyAttrsBits() & RVV_VTA) &&
         (RVVI->getPolicyAttrsBits() & RVV_VMA)) ||
        (!RVVI->isMasked() && (RVVI->getPolicyAttrsBits() & RVV_VTA));
    return RVVI->isMasked() ? NoPassthru ? 1 : 2 : NoPassthru ? 0 : 1;
  }
  if (IRName.starts_with("vsoxseg") || IRName.starts_with("vsuxseg"))
    return RVVI->isMasked() ? 1 : 0;

  return (unsigned)-1;
}

// This function is used to get the log2SEW of each segment load/store, this
// prevent to add a member to RVVIntrinsic.
static unsigned getSegInstLog2SEW(StringRef InstName) {
  // clang-format off
  // We need a special rule for indexed segment load/store since the data width
  // is not encoded in the intrinsic name itself.
  if (InstName.starts_with("vloxseg") || InstName.starts_with("vluxseg") ||
      InstName.starts_with("vsoxseg") || InstName.starts_with("vsuxseg"))
    return (unsigned)-1;

#define KEY_VAL(KEY, VAL) {#KEY, VAL}
#define KEY_VAL_ALL_W_POLICY(KEY, VAL) \
  KEY_VAL(KEY, VAL),                   \
  KEY_VAL(KEY ## _tu, VAL),            \
  KEY_VAL(KEY ## _tum, VAL),           \
  KEY_VAL(KEY ## _tumu, VAL),          \
  KEY_VAL(KEY ## _mu, VAL)

#define KEY_VAL_ALL_NF_BASE(MACRO_NAME, NAME, SEW, LOG2SEW, FF) \
  MACRO_NAME(NAME ## 2e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 3e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 4e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 5e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 6e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 7e ## SEW ## FF, LOG2SEW), \
  MACRO_NAME(NAME ## 8e ## SEW ## FF, LOG2SEW)

#define KEY_VAL_ALL_NF(NAME, SEW, LOG2SEW) \
  KEY_VAL_ALL_NF_BASE(KEY_VAL_ALL_W_POLICY, NAME, SEW, LOG2SEW,)

#define KEY_VAL_FF_ALL_NF(NAME, SEW, LOG2SEW) \
  KEY_VAL_ALL_NF_BASE(KEY_VAL_ALL_W_POLICY, NAME, SEW, LOG2SEW, ff)

#define KEY_VAL_ALL_NF_SEW_BASE(MACRO_NAME, NAME) \
  MACRO_NAME(NAME, 8, 3),  \
  MACRO_NAME(NAME, 16, 4), \
  MACRO_NAME(NAME, 32, 5), \
  MACRO_NAME(NAME, 64, 6)

#define KEY_VAL_ALL_NF_SEW(NAME) \
  KEY_VAL_ALL_NF_SEW_BASE(KEY_VAL_ALL_NF, NAME)

#define KEY_VAL_FF_ALL_NF_SEW(NAME) \
  KEY_VAL_ALL_NF_SEW_BASE(KEY_VAL_FF_ALL_NF, NAME)
  // clang-format on

  static StringMap<unsigned> SegInsts = {
      KEY_VAL_ALL_NF_SEW(vlseg), KEY_VAL_FF_ALL_NF_SEW(vlseg),
      KEY_VAL_ALL_NF_SEW(vlsseg), KEY_VAL_ALL_NF_SEW(vsseg),
      KEY_VAL_ALL_NF_SEW(vssseg)};

#undef KEY_VAL_ALL_NF_SEW
#undef KEY_VAL_ALL_NF
#undef KEY_VAL

  return SegInsts.lookup(InstName);
}

void emitCodeGenSwitchBody(const RVVIntrinsic *RVVI, raw_ostream &OS) {
  if (!RVVI->getIRName().empty())
    OS << "  ID = Intrinsic::riscv_" + RVVI->getIRName() + ";\n";

  OS << "  PolicyAttrs = " << RVVI->getPolicyAttrsBits() << ";\n";
  OS << "  SegInstSEW = " << getSegInstLog2SEW(RVVI->getOverloadedName())
     << ";\n";

  if (RVVI->hasManualCodegen()) {
    OS << "IsMasked = " << (RVVI->isMasked() ? "true" : "false") << ";\n";

    // Skip the non-indexed load/store and compatible header load/store.
    OS << "if (SegInstSEW == (unsigned)-1) {\n";
    OS << "  auto PointeeType = E->getArg(" << getIndexedLoadStorePtrIdx(RVVI)
       << "      )->getType()->getPointeeType();\n";
    OS << "  SegInstSEW = "
          "      llvm::Log2_64(getContext().getTypeSize(PointeeType));\n}\n";

    OS << RVVI->getManualCodegen();
    OS << "break;\n";
    return;
  }

  for (const auto &I : enumerate(RVVI->getInputTypes())) {
    if (I.value()->isPointer()) {
      assert(RVVI->getIntrinsicTypes().front() == -1 &&
             "RVVI should be vector load intrinsic.");
    }
  }

  if (RVVI->isMasked()) {
    if (RVVI->hasVL()) {
      OS << "  std::rotate(Ops.begin(), Ops.begin() + 1, Ops.end() - 1);\n";
      if (RVVI->hasPolicyOperand())
        OS << "  Ops.push_back(ConstantInt::get(Ops.back()->getType(),"
              " PolicyAttrs));\n";
      if (RVVI->hasMaskedOffOperand() && RVVI->getPolicyAttrs().isTAMAPolicy())
        OS << "  Ops.insert(Ops.begin(), "
              "llvm::PoisonValue::get(ResultType));\n";
      // Masked reduction cases.
      if (!RVVI->hasMaskedOffOperand() && RVVI->hasPassthruOperand() &&
          RVVI->getPolicyAttrs().isTAMAPolicy())
        OS << "  Ops.insert(Ops.begin(), "
              "llvm::PoisonValue::get(ResultType));\n";
    } else {
      OS << "  std::rotate(Ops.begin(), Ops.begin() + 1, Ops.end());\n";
    }
  } else {
    if (RVVI->hasPolicyOperand())
      OS << "  Ops.push_back(ConstantInt::get(Ops.back()->getType(), "
            "PolicyAttrs));\n";
    else if (RVVI->hasPassthruOperand() && RVVI->getPolicyAttrs().isTAPolicy())
      OS << "  Ops.insert(Ops.begin(), llvm::PoisonValue::get(ResultType));\n";
  }

  OS << "  IntrinsicTypes = {";
  ListSeparator LS;
  for (const auto &Idx : RVVI->getIntrinsicTypes()) {
    if (Idx == -1)
      OS << LS << "ResultType";
    else
      OS << LS << "Ops[" << Idx << "]->getType()";
  }

  // VL could be i64 or i32, need to encode it in IntrinsicTypes. VL is
  // always last operand.
  if (RVVI->hasVL())
    OS << ", Ops.back()->getType()";
  OS << "};\n";
  OS << "  break;\n";
}

//===----------------------------------------------------------------------===//
// SemaSignatureTable implementation
//===----------------------------------------------------------------------===//
void SemaSignatureTable::init(ArrayRef<SemaRecord> SemaRecords) {
  // Sort signature entries by length, let longer signature insert first, to
  // make it more possible to reuse table entries, that can reduce ~10% table
  // size.
  struct Compare {
    bool operator()(const SmallVector<PrototypeDescriptor> &A,
                    const SmallVector<PrototypeDescriptor> &B) const {
      if (A.size() != B.size())
        return A.size() > B.size();

      size_t Len = A.size();
      for (size_t i = 0; i < Len; ++i) {
        if (A[i] != B[i])
          return A[i] < B[i];
      }

      return false;
    }
  };

  std::set<SmallVector<PrototypeDescriptor>, Compare> Signatures;
  auto InsertToSignatureSet =
      [&](const SmallVector<PrototypeDescriptor> &Signature) {
        if (Signature.empty())
          return;

        Signatures.insert(Signature);
      };

  assert(!SemaRecords.empty());

  for (const SemaRecord &SR : SemaRecords) {
    InsertToSignatureSet(SR.Prototype);
    InsertToSignatureSet(SR.Suffix);
    InsertToSignatureSet(SR.OverloadedSuffix);
  }

  for (auto &Sig : Signatures)
    insert(Sig);
}

void SemaSignatureTable::insert(ArrayRef<PrototypeDescriptor> Signature) {
  if (getIndex(Signature) != INVALID_INDEX)
    return;

  // Insert Signature into SignatureTable if not found in the table.
  SignatureTable.insert(SignatureTable.begin(), Signature.begin(),
                        Signature.end());
}

unsigned SemaSignatureTable::getIndex(ArrayRef<PrototypeDescriptor> Signature) {
  // Empty signature could be point into any index since there is length
  // field when we use, so just always point it to 0.
  if (Signature.empty())
    return 0;

  // Checking Signature already in table or not.
  if (Signature.size() <= SignatureTable.size()) {
    size_t Bound = SignatureTable.size() - Signature.size() + 1;
    for (size_t Index = 0; Index < Bound; ++Index) {
      if (equal(Signature.begin(), Signature.end(),
                SignatureTable.begin() + Index))
        return Index;
    }
  }

  return INVALID_INDEX;
}

void SemaSignatureTable::print(raw_ostream &OS) {
  for (const auto &Sig : SignatureTable)
    OS << "PrototypeDescriptor(" << static_cast<int>(Sig.PT) << ", "
       << static_cast<int>(Sig.VTM) << ", " << static_cast<int>(Sig.TM)
       << "),\n";
}

//===----------------------------------------------------------------------===//
// RVVEmitter implementation
//===----------------------------------------------------------------------===//
void RVVEmitter::createHeader(raw_ostream &OS) {

  OS << "/*===---- riscv_vector.h - RISC-V V-extension RVVIntrinsics "
        "-------------------===\n"
        " *\n"
        " *\n"
        " * Part of the LLVM Project, under the Apache License v2.0 with LLVM "
        "Exceptions.\n"
        " * See https://llvm.org/LICENSE.txt for license information.\n"
        " * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n"
        " *\n"
        " *===-----------------------------------------------------------------"
        "------===\n"
        " */\n\n";

  OS << "#ifndef __RISCV_VECTOR_H\n";
  OS << "#define __RISCV_VECTOR_H\n\n";

  OS << "#include <stdint.h>\n";
  OS << "#include <stddef.h>\n\n";

  OS << "#ifdef __cplusplus\n";
  OS << "extern \"C\" {\n";
  OS << "#endif\n\n";

  OS << "#pragma clang riscv intrinsic vector\n\n";

  printHeaderCode(OS);

  auto printType = [&](auto T) {
    OS << "typedef " << T->getClangBuiltinStr() << " " << T->getTypeStr()
       << ";\n";
  };

  constexpr int Log2LMULs[] = {-3, -2, -1, 0, 1, 2, 3};
  // Print RVV boolean types.
  for (int Log2LMUL : Log2LMULs) {
    auto T = TypeCache.computeType(BasicType::Int8, Log2LMUL,
                                   PrototypeDescriptor::Mask);
    if (T)
      printType(*T);
  }
  // Print RVV int/float types.
  for (char I : StringRef("csil")) {
    BasicType BT = ParseBasicType(I);
    for (int Log2LMUL : Log2LMULs) {
      auto T = TypeCache.computeType(BT, Log2LMUL, PrototypeDescriptor::Vector);
      if (T) {
        printType(*T);
        auto UT = TypeCache.computeType(
            BT, Log2LMUL,
            PrototypeDescriptor(BaseTypeModifier::Vector,
                                VectorTypeModifier::NoModifier,
                                TypeModifier::UnsignedInteger));
        printType(*UT);
      }
      for (int NF = 2; NF <= 8; ++NF) {
        auto TupleT = TypeCache.computeType(
            BT, Log2LMUL,
            PrototypeDescriptor(BaseTypeModifier::Vector, getTupleVTM(NF),
                                TypeModifier::SignedInteger));
        auto TupleUT = TypeCache.computeType(
            BT, Log2LMUL,
            PrototypeDescriptor(BaseTypeModifier::Vector, getTupleVTM(NF),
                                TypeModifier::UnsignedInteger));
        if (TupleT)
          printType(*TupleT);
        if (TupleUT)
          printType(*TupleUT);
      }
    }
  }

  for (BasicType BT : {BasicType::Float16, BasicType::Float32,
                       BasicType::Float64, BasicType::BFloat16}) {
    for (int Log2LMUL : Log2LMULs) {
      auto T = TypeCache.computeType(BT, Log2LMUL, PrototypeDescriptor::Vector);
      if (T)
        printType(*T);
      for (int NF = 2; NF <= 8; ++NF) {
        auto TupleT = TypeCache.computeType(
            BT, Log2LMUL,
            PrototypeDescriptor(BaseTypeModifier::Vector, getTupleVTM(NF),
                                (BT == BasicType::BFloat16
                                     ? TypeModifier::BFloat
                                     : TypeModifier::Float)));
        if (TupleT)
          printType(*TupleT);
      }
    }
  }

  OS << "\n#ifdef __cplusplus\n";
  OS << "}\n";
  OS << "#endif // __cplusplus\n";
  OS << "#endif // __RISCV_VECTOR_H\n";
}

void RVVEmitter::createBuiltins(raw_ostream &OS) {
  std::vector<std::unique_ptr<RVVIntrinsic>> Defs;
  createRVVIntrinsics(Defs);

  llvm::StringToOffsetTable Table;
  // Ensure offset zero is the empty string.
  Table.GetOrAddStringOffset("");
  // Hard coded strings used in the builtin structures.
  Table.GetOrAddStringOffset("n");
  Table.GetOrAddStringOffset("zve32x");

  // Map to unique the builtin names.
  StringMap<RVVIntrinsic *> BuiltinMap;
  std::vector<RVVIntrinsic *> UniqueDefs;
  for (auto &Def : Defs) {
    auto P = BuiltinMap.insert({Def->getBuiltinName(), Def.get()});
    if (P.second) {
      Table.GetOrAddStringOffset(Def->getBuiltinName());
      if (!Def->hasBuiltinAlias())
        Table.GetOrAddStringOffset(Def->getBuiltinTypeStr());
      UniqueDefs.push_back(Def.get());
      continue;
    }

    // Verf that this would have produced the same builtin definition.
    if (P.first->second->hasBuiltinAlias() != Def->hasBuiltinAlias())
      PrintFatalError("Builtin with same name has different hasAutoDef");
    else if (!Def->hasBuiltinAlias() &&
             P.first->second->getBuiltinTypeStr() != Def->getBuiltinTypeStr())
      PrintFatalError("Builtin with same name has different type string");
  }

  // Emit the enumerators of RVV builtins. Note that these are emitted without
  // any outer context to enable concatenating them.
  OS << "// RISCV Vector builtin enumerators\n";
  OS << "#ifdef GET_RISCVV_BUILTIN_ENUMERATORS\n";
  for (RVVIntrinsic *Def : UniqueDefs)
    OS << "  BI__builtin_rvv_" << Def->getBuiltinName() << ",\n";
  OS << "#endif // GET_RISCVV_BUILTIN_ENUMERATORS\n\n";

  // Emit the string table for the RVV builtins.
  OS << "// RISCV Vector builtin enumerators\n";
  OS << "#ifdef GET_RISCVV_BUILTIN_STR_TABLE\n";
  Table.EmitStringTableDef(OS, "BuiltinStrings");
  OS << "#endif // GET_RISCVV_BUILTIN_STR_TABLE\n\n";

  // Emit the info structs of RVV builtins. Note that these are emitted without
  // any outer context to enable concatenating them.
  OS << "// RISCV Vector builtin infos\n";
  OS << "#ifdef GET_RISCVV_BUILTIN_INFOS\n";
  for (RVVIntrinsic *Def : UniqueDefs) {
    OS << "    Builtin::Info{Builtin::Info::StrOffsets{"
       << Table.GetStringOffset(Def->getBuiltinName()) << " /* "
       << Def->getBuiltinName() << " */, ";
    if (Def->hasBuiltinAlias()) {
      OS << "0, ";
    } else {
      OS << Table.GetStringOffset(Def->getBuiltinTypeStr()) << " /* "
         << Def->getBuiltinTypeStr() << " */, ";
    }
    OS << Table.GetStringOffset("n") << " /* n */, ";
    OS << Table.GetStringOffset("zve32x") << " /* zve32x */}, ";

    OS << "HeaderDesc::NO_HEADER, ALL_LANGUAGES},\n";
  }
  OS << "#endif // GET_RISCVV_BUILTIN_INFOS\n\n";
}

void RVVEmitter::createCodeGen(raw_ostream &OS) {
  std::vector<std::unique_ptr<RVVIntrinsic>> Defs;
  createRVVIntrinsics(Defs);
  // IR name could be empty, use the stable sort preserves the relative order.
  stable_sort(Defs, [](const std::unique_ptr<RVVIntrinsic> &A,
                       const std::unique_ptr<RVVIntrinsic> &B) {
    if (A->getIRName() == B->getIRName())
      return (A->getPolicyAttrs() < B->getPolicyAttrs());
    return (A->getIRName() < B->getIRName());
  });

  // Map to keep track of which builtin names have already been emitted.
  StringMap<RVVIntrinsic *> BuiltinMap;

  // Print switch body when the ir name, ManualCodegen, policy or log2sew
  // changes from previous iteration.
  RVVIntrinsic *PrevDef = Defs.begin()->get();
  for (auto &Def : Defs) {
    StringRef CurIRName = Def->getIRName();
    if (CurIRName != PrevDef->getIRName() ||
        (Def->getManualCodegen() != PrevDef->getManualCodegen()) ||
        (Def->getPolicyAttrs() != PrevDef->getPolicyAttrs()) ||
        (getSegInstLog2SEW(Def->getOverloadedName()) !=
         getSegInstLog2SEW(PrevDef->getOverloadedName()))) {
      emitCodeGenSwitchBody(PrevDef, OS);
    }
    PrevDef = Def.get();

    auto P =
        BuiltinMap.insert(std::make_pair(Def->getBuiltinName(), Def.get()));
    if (P.second) {
      OS << "case RISCVVector::BI__builtin_rvv_" << Def->getBuiltinName()
         << ":\n";
      continue;
    }

    if (P.first->second->getIRName() != Def->getIRName())
      PrintFatalError("Builtin with same name has different IRName");
    else if (P.first->second->getManualCodegen() != Def->getManualCodegen())
      PrintFatalError("Builtin with same name has different ManualCodegen");
    else if (P.first->second->isMasked() != Def->isMasked())
      PrintFatalError("Builtin with same name has different isMasked");
    else if (P.first->second->hasVL() != Def->hasVL())
      PrintFatalError("Builtin with same name has different hasVL");
    else if (P.first->second->getPolicyScheme() != Def->getPolicyScheme())
      PrintFatalError("Builtin with same name has different getPolicyScheme");
    else if (P.first->second->getIntrinsicTypes() != Def->getIntrinsicTypes())
      PrintFatalError("Builtin with same name has different IntrinsicTypes");
  }
  emitCodeGenSwitchBody(Defs.back().get(), OS);
  OS << "\n";
}

void RVVEmitter::createRVVIntrinsics(
    std::vector<std::unique_ptr<RVVIntrinsic>> &Out,
    std::vector<SemaRecord> *SemaRecords) {
  for (const Record *R : Records.getAllDerivedDefinitions("RVVBuiltin")) {
    StringRef Name = R->getValueAsString("Name");
    StringRef SuffixProto = R->getValueAsString("Suffix");
    StringRef OverloadedName = R->getValueAsString("OverloadedName");
    StringRef OverloadedSuffixProto = R->getValueAsString("OverloadedSuffix");
    StringRef Prototypes = R->getValueAsString("Prototype");
    StringRef TypeRange = R->getValueAsString("TypeRange");
    bool HasMasked = R->getValueAsBit("HasMasked");
    bool HasMaskedOffOperand = R->getValueAsBit("HasMaskedOffOperand");
    bool HasVL = R->getValueAsBit("HasVL");
    const Record *MPSRecord = R->getValueAsDef("MaskedPolicyScheme");
    auto MaskedPolicyScheme =
        static_cast<PolicyScheme>(MPSRecord->getValueAsInt("Value"));
    const Record *UMPSRecord = R->getValueAsDef("UnMaskedPolicyScheme");
    auto UnMaskedPolicyScheme =
        static_cast<PolicyScheme>(UMPSRecord->getValueAsInt("Value"));
    std::vector<int64_t> Log2LMULList = R->getValueAsListOfInts("Log2LMUL");
    bool HasTailPolicy = R->getValueAsBit("HasTailPolicy");
    bool HasMaskPolicy = R->getValueAsBit("HasMaskPolicy");
    bool SupportOverloading = R->getValueAsBit("SupportOverloading");
    bool HasBuiltinAlias = R->getValueAsBit("HasBuiltinAlias");
    StringRef ManualCodegen = R->getValueAsString("ManualCodegen");
    std::vector<int64_t> IntrinsicTypes =
        R->getValueAsListOfInts("IntrinsicTypes");
    std::vector<StringRef> RequiredFeatures =
        R->getValueAsListOfStrings("RequiredFeatures");
    StringRef IRName = R->getValueAsString("IRName");
    StringRef MaskedIRName = R->getValueAsString("MaskedIRName");
    unsigned NF = R->getValueAsInt("NF");
    bool IsTuple = R->getValueAsBit("IsTuple");
    bool HasFRMRoundModeOp = R->getValueAsBit("HasFRMRoundModeOp");

    const Policy DefaultPolicy;
    SmallVector<Policy> SupportedUnMaskedPolicies =
        RVVIntrinsic::getSupportedUnMaskedPolicies();
    SmallVector<Policy> SupportedMaskedPolicies =
        RVVIntrinsic::getSupportedMaskedPolicies(HasTailPolicy, HasMaskPolicy);

    // Parse prototype and create a list of primitive type with transformers
    // (operand) in Prototype. Prototype[0] is output operand.
    SmallVector<PrototypeDescriptor> BasicPrototype =
        parsePrototypes(Prototypes);

    SmallVector<PrototypeDescriptor> SuffixDesc = parsePrototypes(SuffixProto);
    SmallVector<PrototypeDescriptor> OverloadedSuffixDesc =
        parsePrototypes(OverloadedSuffixProto);

    // Compute Builtin types
    auto Prototype = RVVIntrinsic::computeBuiltinTypes(
        BasicPrototype, /*IsMasked=*/false,
        /*HasMaskedOffOperand=*/false, HasVL, NF, UnMaskedPolicyScheme,
        DefaultPolicy, IsTuple);
    SmallVector<PrototypeDescriptor> MaskedPrototype;
    if (HasMasked)
      MaskedPrototype = RVVIntrinsic::computeBuiltinTypes(
          BasicPrototype, /*IsMasked=*/true, HasMaskedOffOperand, HasVL, NF,
          MaskedPolicyScheme, DefaultPolicy, IsTuple);

    // Create Intrinsics for each type and LMUL.
    for (char I : TypeRange) {
      for (int Log2LMUL : Log2LMULList) {
        BasicType BT = ParseBasicType(I);
        std::optional<RVVTypes> Types =
            TypeCache.computeTypes(BT, Log2LMUL, NF, Prototype);
        // Ignored to create new intrinsic if there are any illegal types.
        if (!Types)
          continue;

        auto SuffixStr =
            RVVIntrinsic::getSuffixStr(TypeCache, BT, Log2LMUL, SuffixDesc);
        auto OverloadedSuffixStr = RVVIntrinsic::getSuffixStr(
            TypeCache, BT, Log2LMUL, OverloadedSuffixDesc);
        // Create a unmasked intrinsic
        Out.push_back(std::make_unique<RVVIntrinsic>(
            Name, SuffixStr, OverloadedName, OverloadedSuffixStr, IRName,
            /*IsMasked=*/false, /*HasMaskedOffOperand=*/false, HasVL,
            UnMaskedPolicyScheme, SupportOverloading, HasBuiltinAlias,
            ManualCodegen, *Types, IntrinsicTypes, NF, DefaultPolicy,
            HasFRMRoundModeOp));
        if (UnMaskedPolicyScheme != PolicyScheme::SchemeNone)
          for (auto P : SupportedUnMaskedPolicies) {
            SmallVector<PrototypeDescriptor> PolicyPrototype =
                RVVIntrinsic::computeBuiltinTypes(
                    BasicPrototype, /*IsMasked=*/false,
                    /*HasMaskedOffOperand=*/false, HasVL, NF,
                    UnMaskedPolicyScheme, P, IsTuple);
            std::optional<RVVTypes> PolicyTypes =
                TypeCache.computeTypes(BT, Log2LMUL, NF, PolicyPrototype);
            Out.push_back(std::make_unique<RVVIntrinsic>(
                Name, SuffixStr, OverloadedName, OverloadedSuffixStr, IRName,
                /*IsMask=*/false, /*HasMaskedOffOperand=*/false, HasVL,
                UnMaskedPolicyScheme, SupportOverloading, HasBuiltinAlias,
                ManualCodegen, *PolicyTypes, IntrinsicTypes, NF, P,
                HasFRMRoundModeOp));
          }
        if (!HasMasked)
          continue;
        // Create a masked intrinsic
        std::optional<RVVTypes> MaskTypes =
            TypeCache.computeTypes(BT, Log2LMUL, NF, MaskedPrototype);
        Out.push_back(std::make_unique<RVVIntrinsic>(
            Name, SuffixStr, OverloadedName, OverloadedSuffixStr, MaskedIRName,
            /*IsMasked=*/true, HasMaskedOffOperand, HasVL, MaskedPolicyScheme,
            SupportOverloading, HasBuiltinAlias, ManualCodegen, *MaskTypes,
            IntrinsicTypes, NF, DefaultPolicy, HasFRMRoundModeOp));
        if (MaskedPolicyScheme == PolicyScheme::SchemeNone)
          continue;
        for (auto P : SupportedMaskedPolicies) {
          SmallVector<PrototypeDescriptor> PolicyPrototype =
              RVVIntrinsic::computeBuiltinTypes(
                  BasicPrototype, /*IsMasked=*/true, HasMaskedOffOperand, HasVL,
                  NF, MaskedPolicyScheme, P, IsTuple);
          std::optional<RVVTypes> PolicyTypes =
              TypeCache.computeTypes(BT, Log2LMUL, NF, PolicyPrototype);
          Out.push_back(std::make_unique<RVVIntrinsic>(
              Name, SuffixStr, OverloadedName, OverloadedSuffixStr,
              MaskedIRName, /*IsMasked=*/true, HasMaskedOffOperand, HasVL,
              MaskedPolicyScheme, SupportOverloading, HasBuiltinAlias,
              ManualCodegen, *PolicyTypes, IntrinsicTypes, NF, P,
              HasFRMRoundModeOp));
        }
      } // End for Log2LMULList
    }   // End for TypeRange

    // We don't emit vsetvli and vsetvlimax for SemaRecord.
    // They are written in riscv_vector.td and will emit those marco define in
    // riscv_vector.h
    if (Name == "vsetvli" || Name == "vsetvlimax")
      continue;

    if (!SemaRecords)
      continue;

    // Create SemaRecord
    SemaRecord SR;
    SR.Name = Name.str();
    SR.OverloadedName = OverloadedName.str();
    BasicType TypeRangeMask = BasicType::Unknown;
    for (char I : TypeRange)
      TypeRangeMask |= ParseBasicType(I);

    SR.TypeRangeMask = static_cast<unsigned>(TypeRangeMask);

    unsigned Log2LMULMask = 0;
    for (int Log2LMUL : Log2LMULList)
      Log2LMULMask |= 1 << (Log2LMUL + 3);

    SR.Log2LMULMask = Log2LMULMask;
    std::string RFs =
        join(RequiredFeatures.begin(), RequiredFeatures.end(), ",");
    SR.RequiredExtensions = RFs;
    SR.NF = NF;
    SR.HasMasked = HasMasked;
    SR.HasVL = HasVL;
    SR.HasMaskedOffOperand = HasMaskedOffOperand;
    SR.HasTailPolicy = HasTailPolicy;
    SR.HasMaskPolicy = HasMaskPolicy;
    SR.UnMaskedPolicyScheme = static_cast<uint8_t>(UnMaskedPolicyScheme);
    SR.MaskedPolicyScheme = static_cast<uint8_t>(MaskedPolicyScheme);
    SR.Prototype = std::move(BasicPrototype);
    SR.Suffix = parsePrototypes(SuffixProto);
    SR.OverloadedSuffix = parsePrototypes(OverloadedSuffixProto);
    SR.IsTuple = IsTuple;
    SR.HasFRMRoundModeOp = HasFRMRoundModeOp;

    SemaRecords->push_back(SR);
  }
}

void RVVEmitter::printHeaderCode(raw_ostream &OS) {
  for (const Record *R : Records.getAllDerivedDefinitions("RVVHeader")) {
    StringRef HeaderCodeStr = R->getValueAsString("HeaderCode");
    OS << HeaderCodeStr.str();
  }
}

void RVVEmitter::createRVVIntrinsicRecords(std::vector<RVVIntrinsicRecord> &Out,
                                           SemaSignatureTable &SST,
                                           ArrayRef<SemaRecord> SemaRecords) {
  SST.init(SemaRecords);

  for (const auto &SR : SemaRecords) {
    Out.emplace_back(RVVIntrinsicRecord());
    RVVIntrinsicRecord &R = Out.back();
    R.Name = SR.Name.c_str();
    R.OverloadedName = SR.OverloadedName.c_str();
    R.PrototypeIndex = SST.getIndex(SR.Prototype);
    R.SuffixIndex = SST.getIndex(SR.Suffix);
    R.OverloadedSuffixIndex = SST.getIndex(SR.OverloadedSuffix);
    R.PrototypeLength = SR.Prototype.size();
    R.SuffixLength = SR.Suffix.size();
    R.OverloadedSuffixSize = SR.OverloadedSuffix.size();
    R.RequiredExtensions = SR.RequiredExtensions.c_str();
    R.TypeRangeMask = SR.TypeRangeMask;
    R.Log2LMULMask = SR.Log2LMULMask;
    R.NF = SR.NF;
    R.HasMasked = SR.HasMasked;
    R.HasVL = SR.HasVL;
    R.HasMaskedOffOperand = SR.HasMaskedOffOperand;
    R.HasTailPolicy = SR.HasTailPolicy;
    R.HasMaskPolicy = SR.HasMaskPolicy;
    R.UnMaskedPolicyScheme = SR.UnMaskedPolicyScheme;
    R.MaskedPolicyScheme = SR.MaskedPolicyScheme;
    R.IsTuple = SR.IsTuple;
    R.HasFRMRoundModeOp = SR.HasFRMRoundModeOp;

    assert(R.PrototypeIndex !=
           static_cast<uint16_t>(SemaSignatureTable::INVALID_INDEX));
    assert(R.SuffixIndex !=
           static_cast<uint16_t>(SemaSignatureTable::INVALID_INDEX));
    assert(R.OverloadedSuffixIndex !=
           static_cast<uint16_t>(SemaSignatureTable::INVALID_INDEX));
  }
}

void RVVEmitter::createSema(raw_ostream &OS) {
  std::vector<std::unique_ptr<RVVIntrinsic>> Defs;
  std::vector<RVVIntrinsicRecord> RVVIntrinsicRecords;
  SemaSignatureTable SST;
  std::vector<SemaRecord> SemaRecords;

  createRVVIntrinsics(Defs, &SemaRecords);

  createRVVIntrinsicRecords(RVVIntrinsicRecords, SST, SemaRecords);

  // Emit signature table for SemaRISCVVectorLookup.cpp.
  OS << "#ifdef DECL_SIGNATURE_TABLE\n";
  SST.print(OS);
  OS << "#endif\n";

  // Emit RVVIntrinsicRecords for SemaRISCVVectorLookup.cpp.
  OS << "#ifdef DECL_INTRINSIC_RECORDS\n";
  for (const RVVIntrinsicRecord &Record : RVVIntrinsicRecords)
    OS << Record;
  OS << "#endif\n";
}

namespace clang {
void EmitRVVHeader(const RecordKeeper &Records, raw_ostream &OS) {
  RVVEmitter(Records).createHeader(OS);
}

void EmitRVVBuiltins(const RecordKeeper &Records, raw_ostream &OS) {
  RVVEmitter(Records).createBuiltins(OS);
}

void EmitRVVBuiltinCG(const RecordKeeper &Records, raw_ostream &OS) {
  RVVEmitter(Records).createCodeGen(OS);
}

void EmitRVVBuiltinSema(const RecordKeeper &Records, raw_ostream &OS) {
  RVVEmitter(Records).createSema(OS);
}

} // End namespace clang
