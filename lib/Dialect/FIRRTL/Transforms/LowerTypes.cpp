//===- LowerTypes.cpp - Lower Aggregate Types -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the LowerTypes pass.  This pass replaces aggregate types
// with expanded values.
//
// This pass walks the operations in reverse order. This lets it visit users
// before defs. Users can usually be expanded out to multiple operations (think
// mux of a bundle to muxes of each field) with a temporary subWhatever op
// inserted. When processing an aggregate producer, we blow out the op as
// appropriate, then walk the users, often those are subWhatever ops which can
// be bypassed and deleted. Function arguments are logically last on the
// operation visit order and walked left to right, being peeled one layer at a
// time with replacements inserted to the right of the original argument.
//
// Each processing of an op peels one layer of aggregate type off.  Because new
// ops are inserted immediately above the current up, the walk will visit them
// next, effectively recusing on the aggregate types, without recusing.  These
// potentially temporary ops(if the aggregate is complex) effectively serve as
// the worklist.  Often aggregates are shallow, so the new ops are the final
// ones.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/NLATable.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Threading.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Parallel.h"

#define DEBUG_TYPE "firrtl-lower-types"

using namespace circt;
using namespace firrtl;

// TODO: check all argument types
namespace {
/// This represents a flattened bundle field element.
struct FlatBundleFieldEntry {
  /// This is the underlying ground type of the field.
  FIRRTLType type;
  /// The index in the parent type
  size_t index;
  /// The fieldID
  unsigned fieldID;
  /// This is a suffix to add to the field name to make it unique.
  SmallString<16> suffix;
  /// This indicates whether the field was flipped to be an output.
  bool isOutput;

  FlatBundleFieldEntry(const FIRRTLType &type, size_t index, unsigned fieldID,
                       StringRef suffix, bool isOutput)
      : type(type), index(index), fieldID(fieldID), suffix(suffix),
        isOutput(isOutput) {}

  void dump() const {
    llvm::errs() << "FBFE{" << type << " index<" << index << "> fieldID<"
                 << fieldID << "> suffix<" << suffix << "> isOutput<"
                 << isOutput << ">}\n";
  }
};
} // end anonymous namespace

/// Return true if the type has more than zero bitwidth.
static bool hasZeroBitWidth(FIRRTLType type) {
  return TypeSwitch<FIRRTLType, bool>(type)
      .Case<BundleType>([&](auto bundle) {
        for (size_t i = 0, e = bundle.getNumElements(); i < e; ++i) {
          auto elt = bundle.getElement(i);
          if (hasZeroBitWidth(elt.type))
            return true;
        }
        return bundle.getNumElements() == 0;
      })
      .Case<FVectorType>([&](auto vector) {
        if (vector.getNumElements() == 0)
          return true;
        return hasZeroBitWidth(vector.getElementType());
      })
      .Default([](auto groundType) {
        return firrtl::getBitWidth(groundType).getValueOr(0) == 0;
      });
}

/// Return true if we can preserve the aggregate type. We can a preserve the
/// type iff (i) the type is not passive, (ii) the type doesn't contain analog
/// and (iii) type don't contain zero bitwidth.
static bool isPreservableAggregateType(Type type) {
  auto firrtlType = type.cast<FIRRTLType>();
  return firrtlType.isPassive() && !firrtlType.containsAnalog() &&
         !hasZeroBitWidth(firrtlType);
}

/// Peel one layer of an aggregate type into its components.  Type may be
/// complex, but empty, in which case fields is empty, but the return is true.
static bool peelType(Type type, SmallVectorImpl<FlatBundleFieldEntry> &fields,
                     bool allowedToPreserveAggregate = false) {
  // If the aggregate preservation is enabled and the type is preservable,
  // then just return.
  if (allowedToPreserveAggregate && isPreservableAggregateType(type))
    return false;

  return TypeSwitch<Type, bool>(type)
      .Case<BundleType>([&](auto bundle) {
        SmallString<16> tmpSuffix;
        // Otherwise, we have a bundle type.  Break it down.
        for (size_t i = 0, e = bundle.getNumElements(); i < e; ++i) {
          auto elt = bundle.getElement(i);
          // Construct the suffix to pass down.
          tmpSuffix.resize(0);
          tmpSuffix.push_back('_');
          tmpSuffix.append(elt.name.getValue());
          fields.emplace_back(elt.type, i, bundle.getFieldID(i), tmpSuffix,
                              elt.isFlip);
        }
        return true;
      })
      .Case<FVectorType>([&](auto vector) {
        // Increment the field ID to point to the first element.
        for (size_t i = 0, e = vector.getNumElements(); i != e; ++i) {
          fields.emplace_back(vector.getElementType(), i, vector.getFieldID(i),
                              "_" + std::to_string(i), false);
        }
        return true;
      })
      .Default([](auto op) { return false; });
}

/// Return if something is not a normal subaccess.  Non-normal includes
/// zero-length vectors and constant indexes (which are really subindexes).
static bool isNotSubAccess(Operation *op) {
  SubaccessOp sao = dyn_cast<SubaccessOp>(op);
  if (!sao)
    return true;
  ConstantOp arg = dyn_cast_or_null<ConstantOp>(sao.index().getDefiningOp());
  if (arg && sao.input().getType().cast<FVectorType>().getNumElements() != 0)
    return true;
  return false;
}

/// Look through and collect subfields leading to a subaccess.
static SmallVector<Operation *> getSAWritePath(Operation *op) {
  SmallVector<Operation *> retval;
  auto defOp = op->getOperand(0).getDefiningOp();
  while (isa_and_nonnull<SubfieldOp, SubindexOp, SubaccessOp>(defOp)) {
    retval.push_back(defOp);
    defOp = defOp->getOperand(0).getDefiningOp();
  }
  // Trim to the subaccess
  while (!retval.empty() && isNotSubAccess(retval.back()))
    retval.pop_back();
  return retval;
}

/// Returns whether the given annotation requires precise tracking of the field
/// ID as it gets replicated across lowered operations.
static bool isAnnotationSensitiveToFieldID(Annotation anno) {
  return anno.isClass("sifive.enterprise.grandcentral.SignalDriverAnnotation");
}

/// If an annotation on one operation is replicated across multiple IR
/// operations as a result of type lowering, the replicated annotations may want
/// to track which field ID they were applied to. This function adds a fieldID
/// to such a replicated operation, if the annotation in question requires it.
static Attribute updateAnnotationFieldID(MLIRContext *ctxt, Attribute attr,
                                         unsigned fieldID, Type i64ty) {
  DictionaryAttr dict = attr.cast<DictionaryAttr>();

  // No need to do anything if the annotation applies to the entire field.
  if (fieldID == 0)
    return attr;

  // Only certain annotations require precise tracking of field IDs.
  Annotation anno(dict);
  if (!isAnnotationSensitiveToFieldID(anno))
    return attr;

  // Add the new ID to the existing field ID in the annotation.
  if (auto existingFieldID = anno.getMember<IntegerAttr>("fieldID"))
    fieldID += existingFieldID.getValue().getZExtValue();
  NamedAttrList fields(dict);
  fields.set("fieldID", IntegerAttr::get(i64ty, fieldID));
  return DictionaryAttr::get(ctxt, fields);
}

static MemOp cloneMemWithNewType(ImplicitLocOpBuilder *b, MemOp op,
                                 FlatBundleFieldEntry field) {
  SmallVector<Type, 8> ports;
  SmallVector<Attribute, 8> portNames;

  auto oldPorts = op.getPorts();
  for (size_t portIdx = 0, e = oldPorts.size(); portIdx < e; ++portIdx) {
    auto port = oldPorts[portIdx];
    ports.push_back(MemOp::getTypeForPort(op.depth(), field.type, port.second));
    portNames.push_back(port.first);
  }

  // It's easier to duplicate the old annotations, then fix and filter them.
  auto newMem =
      b->create<MemOp>(ports, op.readLatency(), op.writeLatency(), op.depth(),
                       op.ruw(), portNames, (op.name() + field.suffix).str(),
                       op.nameKind(), op.annotations().getValue(),
                       op.portAnnotations().getValue(), op.inner_symAttr());
  if (auto oldName = getInnerSymName(op))
    newMem.inner_symAttr(InnerSymAttr::get(StringAttr::get(
        b->getContext(), oldName.getValue() + (op.name() + field.suffix))));

  SmallVector<Attribute> newAnnotations;
  for (size_t portIdx = 0, e = newMem.getNumResults(); portIdx < e; ++portIdx) {
    auto portType = newMem.getResult(portIdx).getType().cast<BundleType>();
    auto oldPortType = op.getResult(portIdx).getType().cast<BundleType>();
    SmallVector<Attribute> portAnno;
    for (auto attr : newMem.getPortAnnotation(portIdx)) {
      Annotation anno(attr);
      if (auto annoFieldID = anno.getFieldID()) {
        auto targetIndex = oldPortType.getIndexForFieldID(annoFieldID);

        // Apply annotations to all elements if the target is the whole
        // sub-field.
        if (annoFieldID == oldPortType.getFieldID(targetIndex)) {
          anno.setMember(
              "circt.fieldID",
              b->getI32IntegerAttr(portType.getFieldID(targetIndex)));
          portAnno.push_back(anno.getDict());
          continue;
        }

        // Handle aggregate sub-fields, including `(r/w)data` and `(w)mask`.
        if (oldPortType.getElement(targetIndex).type.isa<BundleType>()) {
          // Check whether the annotation falls into the range of the current
          // field. Note that the `field` here is peeled from the `data`
          // sub-field of the memory port, thus we need to add the fieldID of
          // `data` or `mask` sub-field to get the "real" fieldID.
          auto fieldID = field.fieldID + oldPortType.getFieldID(targetIndex);
          if (annoFieldID >= fieldID &&
              annoFieldID <= fieldID + field.type.getMaxFieldID()) {
            // Set the field ID of the new annotation.
            auto newFieldID =
                annoFieldID - fieldID + portType.getFieldID(targetIndex);
            anno.setMember("circt.fieldID", b->getI32IntegerAttr(newFieldID));
            portAnno.push_back(anno.getDict());
          }
        }
      } else
        portAnno.push_back(attr);
    }
    newAnnotations.push_back(b->getArrayAttr(portAnno));
  }
  newMem.setAllPortAnnotations(newAnnotations);
  return newMem;
}

//===----------------------------------------------------------------------===//
// Module Type Lowering
//===----------------------------------------------------------------------===//
namespace {

struct AttrCache {
  AttrCache(MLIRContext *context) {
    i64ty = IntegerType::get(context, 64);
    innerSymAttr = StringAttr::get(context, "inner_sym");
    nameAttr = StringAttr::get(context, "name");
    nameKindAttr = StringAttr::get(context, "nameKind");
    sPortDirections = StringAttr::get(context, "portDirections");
    sPortNames = StringAttr::get(context, "portNames");
    sPortTypes = StringAttr::get(context, "portTypes");
    sPortSyms = StringAttr::get(context, "portSyms");
    sPortAnnotations = StringAttr::get(context, "portAnnotations");
    sEmpty = StringAttr::get(context, "");
  }
  AttrCache(const AttrCache &) = default;

  Type i64ty;
  StringAttr innerSymAttr, nameAttr, nameKindAttr, sPortDirections, sPortNames,
      sPortTypes, sPortSyms, sPortAnnotations, sEmpty;
};

// The visitors all return true if the operation should be deleted, false if
// not.
struct TypeLoweringVisitor : public FIRRTLVisitor<TypeLoweringVisitor, bool> {

  TypeLoweringVisitor(MLIRContext *context, bool preserveAggregate,
                      bool preservePublicTypes, SymbolTable &symTbl,
                      const AttrCache &cache)
      : context(context), preserveAggregate(preserveAggregate),
        preservePublicTypes(preservePublicTypes), symTbl(symTbl), cache(cache) {
  }
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitDecl;
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitExpr;
  using FIRRTLVisitor<TypeLoweringVisitor, bool>::visitStmt;

  /// If the referenced operation is a FModuleOp or an FExtModuleOp, perform
  /// type lowering on all operations.
  void lowerModule(FModuleLike op);

  bool lowerArg(FModuleLike module, size_t argIndex, size_t argsRemoved,
                SmallVectorImpl<PortInfo> &newArgs,
                SmallVectorImpl<Value> &lowering);
  std::pair<Value, PortInfo> addArg(Operation *module, unsigned insertPt,
                                    unsigned insertPtOffset, FIRRTLType srcType,
                                    FlatBundleFieldEntry field,
                                    PortInfo &oldArg);

  // Helpers to manage state.
  bool visitDecl(FExtModuleOp op);
  bool visitDecl(FModuleOp op);
  bool visitDecl(InstanceOp op);
  bool visitDecl(MemOp op);
  bool visitDecl(NodeOp op);
  bool visitDecl(RegOp op);
  bool visitDecl(WireOp op);
  bool visitDecl(RegResetOp op);
  bool visitExpr(InvalidValueOp op);
  bool visitExpr(SubaccessOp op);
  bool visitExpr(MultibitMuxOp op);
  bool visitExpr(MuxPrimOp op);
  bool visitExpr(mlir::UnrealizedConversionCastOp op);
  bool visitExpr(BitCastOp op);
  bool visitStmt(ConnectOp op);
  bool visitStmt(StrictConnectOp op);
  bool visitStmt(WhenOp op);

  DenseMap<hw::InnerRefAttr, SmallVector<AnnoTarget>> &getRenames() {
    return innerRefRenames;
  };

private:
  void processUsers(Value val, ArrayRef<Value> mapping);
  bool processSAPath(Operation *);
  void lowerBlock(Block *);
  void lowerSAWritePath(Operation *, ArrayRef<Operation *> writePath);
  bool lowerProducer(
      Operation *op,
      llvm::function_ref<Operation *(const FlatBundleFieldEntry &, ArrayAttr)>
          clone);
  /// Copy annotations from \p annotations to \p loweredAttrs, except
  /// annotations with "target" key, that do not match the field suffix. Also if
  /// the target contains a DontTouch, remove it and set the flag.
  ArrayAttr filterAnnotations(MLIRContext *ctxt, ArrayAttr annotations,
                              FIRRTLType srcType, FlatBundleFieldEntry field,
                              bool &needsSym, StringRef sym);

  bool isModuleAllowedToPreserveAggregate(FModuleLike moduleLike);
  Value getSubWhatever(Value val, size_t index);

  size_t uniqueIdx = 0;
  std::string uniqueName() {
    auto myID = uniqueIdx++;
    return (Twine("__GEN_") + Twine(myID)).str();
  }

  MLIRContext *context;

  /// Not to lower passive aggregate types as much as possible if this flag is
  /// enabled.
  bool preserveAggregate;

  /// Exteranal modules and toplevel modules should have lowered types if this
  /// flag is enabled.
  bool preservePublicTypes;

  /// The builder is set and maintained in the main loop.
  ImplicitLocOpBuilder *builder;

  /// Record how a given hw::InnerRefAttr (a tuple of Module Name and Component
  /// Name) are renamed to one or more targets.  The hw::InnerRefAttr always
  /// uses the original inner symbol.  This is done with the assistance of the
  /// origSymbols member below.
  DenseMap<hw::InnerRefAttr, SmallVector<AnnoTarget>> innerRefRenames;

  /// A disjoint-set datastructure consiting of each set of renamed symbols.
  /// The leader is the original symbol.  This is used to recover the original
  /// symbol from any point in the recursive lowering.  This original symbol is
  /// then used to choose the key for innerRefRenames which enables hierarchical
  /// paths (which are updated later and use the original symbol) to be updated
  /// after each module is lowered.
  ///
  /// E.g., if the original wire is:
  ///
  ///     %a = firrtl.wire sym @a !firrtl.bundle<a: uint<1>, b: bundle<c: uint>>
  ///
  /// Then origSymbols will contain a disjoint set, where "a" is the leader:
  ///
  ///     [ "a", "a_a", "a_b", "a_b_c" ]
  ///
  /// Note: this will contain _all intermediary symbols_ that are created during
  /// recursive lowering and not just the final, lowered symbols.  However, only
  /// final renames will be recorded in innerRefRenames because innerRefRenames
  /// is only updated when the type is a ground type.
  llvm::EquivalenceClasses<StringRef> origSymbols;

  // Keep a symbol table around for resolving symbols
  SymbolTable &symTbl;

  // Cache some attributes
  const AttrCache &cache;
};
} // namespace

/// Return true if we can preserve the arguments of the given module.
/// Exteranal modules and toplevel modules are sometimes assumed to have lowered
/// types.
bool TypeLoweringVisitor::isModuleAllowedToPreserveAggregate(
    FModuleLike module) {

  if (!preserveAggregate)
    return false;

  // If it is not forced to lower toplevel and external modules, it's ok to
  // preserve.
  if (!preservePublicTypes)
    return true;

  if (isa<FExtModuleOp>(module))
    return false;
  return !cast<hw::HWModuleLike>(*module).isPublic();
}

Value TypeLoweringVisitor::getSubWhatever(Value val, size_t index) {
  if (BundleType bundle = val.getType().dyn_cast<BundleType>()) {
    return builder->create<SubfieldOp>(val, index);
  } else if (FVectorType fvector = val.getType().dyn_cast<FVectorType>()) {
    return builder->create<SubindexOp>(val, index);
  }
  llvm_unreachable("Unknown aggregate type");
  return nullptr;
}

/// Conditionally expand a subaccessop write path
bool TypeLoweringVisitor::processSAPath(Operation *op) {
  // Does this LHS have a subaccessop?
  SmallVector<Operation *> writePath = getSAWritePath(op);
  if (writePath.empty())
    return false;

  lowerSAWritePath(op, writePath);
  // Unhook the writePath from the connect.  This isn't the right type, but we
  // are deleting the op anyway.
  op->eraseOperands(0, 2);
  // See how far up the tree we can delete things.
  for (size_t i = 0; i < writePath.size(); ++i) {
    if (writePath[i]->use_empty()) {
      writePath[i]->erase();
    } else {
      break;
    }
  }
  return true;
}

void TypeLoweringVisitor::lowerBlock(Block *block) {
  // Lower the operations bottom up.
  for (auto it = block->rbegin(), e = block->rend(); it != e;) {
    auto &iop = *it;
    builder->setInsertionPoint(&iop);
    builder->setLoc(iop.getLoc());
    bool removeOp = dispatchVisitor(&iop);
    ++it;
    // Erase old ops eagerly so we don't have dangling uses we've already
    // lowered.
    if (removeOp)
      iop.erase();
  }
}

ArrayAttr TypeLoweringVisitor::filterAnnotations(
    MLIRContext *ctxt, ArrayAttr annotations, FIRRTLType srcType,
    FlatBundleFieldEntry field, bool &needsSym, StringRef sym) {
  SmallVector<Attribute> retval;
  if (!annotations || annotations.empty())
    return ArrayAttr::get(ctxt, retval);
  bool isGroundType = field.type.isGround();
  for (auto opAttr : annotations) {
    Optional<int64_t> maybeFieldID = None;
    DictionaryAttr annotation;
    annotation = opAttr.dyn_cast<DictionaryAttr>();
    if (annotations)
      // Erase the circt.fieldID.  If this is needed later, it will be re-added.
      if (auto id = annotation.getAs<IntegerAttr>("circt.fieldID")) {
        maybeFieldID = id.getInt();
        Annotation anno(annotation);
        anno.removeMember("circt.fieldID");
        annotation = anno.getDict();
      }
    if (!maybeFieldID) {
      retval.push_back(
          updateAnnotationFieldID(ctxt, opAttr, field.fieldID, cache.i64ty));
      continue;
    }
    auto fieldID = maybeFieldID.getValue();
    // Check whether the annotation falls into the range of the current field.
    if (fieldID != 0 &&
        !(fieldID >= field.fieldID &&
          fieldID <= field.fieldID + field.type.getMaxFieldID()))
      continue;

    // Apply annotations to all elements if fieldID is equal to zero.
    if (fieldID == 0) {
      retval.push_back(annotation);
      continue;
    }

    if (auto newFieldID = fieldID - field.fieldID) {
      // If the target is a subfield/subindex of the current field, create a
      // new annotation with the correct circt.fieldID.
      Annotation newAnno(annotation);
      newAnno.setMember("circt.fieldID",
                        builder->getI32IntegerAttr(newFieldID));
      retval.push_back(newAnno.getDict());
      continue;
    }
    if (Annotation(opAttr).getClass() ==
        "firrtl.transforms.DontTouchAnnotation") {
      // This is intended to cover the case of a non-local DontTouchAnnotation
      // (which is represented as an annotation) being converted to a symbol on
      // a ground type.  This code will, however, also lower any local
      // DontTouchAnnotation (even though this should not exist at this point).
      needsSym = true;
      continue;
    }
    // We are keeping the annotation.  If the anotation is non-local and this is
    // a ground type (this won't be further lowered) then generate a symbol.
    needsSym =
        isGroundType && annotation.getAs<FlatSymbolRefAttr>("circt.nonlocal");
    retval.push_back(annotation);
  }
  return ArrayAttr::get(ctxt, retval);
}

bool TypeLoweringVisitor::lowerProducer(
    Operation *op,
    llvm::function_ref<Operation *(const FlatBundleFieldEntry &, ArrayAttr)>
        clone) {
  // If this is not a bundle, there is nothing to do.
  auto srcType = op->getResult(0).getType().cast<FIRRTLType>();
  SmallVector<FlatBundleFieldEntry, 8> fieldTypes;

  if (!peelType(srcType, fieldTypes, preserveAggregate))
    return false;

  SmallVector<Value> lowered;
  // Loop over the leaf aggregates.
  SmallString<16> loweredName;
  SmallString<16> loweredSymName;
  auto nameKindAttr = op->getAttrOfType<NameKindEnumAttr>(cache.nameKindAttr);

  auto innerSymAttr = getInnerSymName(op);
  if (innerSymAttr)
    loweredSymName = innerSymAttr.getValue();
  if (auto nameAttr = op->getAttrOfType<StringAttr>(cache.nameAttr))
    loweredName = nameAttr.getValue();
  if (loweredSymName.empty())
    loweredSymName = loweredName;
  if (loweredSymName.empty())
    loweredSymName = uniqueName();
  auto baseNameLen = loweredName.size();
  auto baseSymNameLen = loweredSymName.size();
  auto oldAnno = op->getAttr("annotations").dyn_cast_or_null<ArrayAttr>();

  for (auto field : fieldTypes) {
    if (!loweredName.empty()) {
      loweredName.resize(baseNameLen);
      loweredName += field.suffix;
    }
    if (!loweredSymName.empty()) {
      loweredSymName.resize(baseSymNameLen);
      loweredSymName += field.suffix;
    }
    bool needsSym = false;

    // For all annotations on the parent op, filter them based on the target
    // attribute.
    ArrayAttr loweredAttrs = filterAnnotations(context, oldAnno, srcType, field,
                                               needsSym, loweredSymName);
    auto *newOp = clone(field, loweredAttrs);

    // Carry over the name, if present.
    if (!loweredName.empty())
      newOp->setAttr(cache.nameAttr, StringAttr::get(context, loweredName));
    if (nameKindAttr)
      newOp->setAttr(cache.nameKindAttr, nameKindAttr);
    // Carry over the inner_sym name, if present.
    if (needsSym || op->hasAttr(cache.innerSymAttr)) {
      auto newName = StringAttr::get(context, loweredSymName);
      newOp->setAttr(cache.innerSymAttr, InnerSymAttr::get(newName));
      assert(!loweredSymName.empty());

      // If this operation has an inner symbol, then update the origSymbols
      // disjoint set to make sure that all derived symbols are associated with
      // the original symbol.
      if (innerSymAttr) {
        origSymbols.unionSets(innerSymAttr.getValue(), newName.getValue());
        if (field.type.isGround()) {
          auto module = op->getParentOfType<FModuleOp>();
          auto key = origSymbols.findLeader(innerSymAttr.getValue());
          StringAttr keyAttr = StringAttr::get(module.getContext(), *key);
          innerRefRenames[hw::InnerRefAttr::get(module.getNameAttr(), keyAttr)]
              .push_back(OpAnnoTarget(newOp));
        }
      }
    }
    lowered.push_back(newOp->getResult(0));
  }

  processUsers(op->getResult(0), lowered);
  return true;
}

void TypeLoweringVisitor::processUsers(Value val, ArrayRef<Value> mapping) {
  for (auto user : llvm::make_early_inc_range(val.getUsers())) {
    if (SubindexOp sio = dyn_cast<SubindexOp>(user)) {
      Value repl = mapping[sio.index()];
      sio.replaceAllUsesWith(repl);
      sio.erase();
    } else if (SubfieldOp sfo = dyn_cast<SubfieldOp>(user)) {
      // Get the input bundle type.
      Value repl = mapping[sfo.fieldIndex()];
      sfo.replaceAllUsesWith(repl);
      sfo.erase();
    } else {
      val.dump();
      val.getDefiningOp()->getParentOfType<FModuleOp>()->dump();
      llvm_unreachable("Unknown aggregate user");
    }
  }
}

void TypeLoweringVisitor::lowerModule(FModuleLike op) {
  if (auto module = dyn_cast<FModuleOp>(*op))
    visitDecl(module);
  else if (auto extModule = dyn_cast<FExtModuleOp>(*op))
    visitDecl(extModule);
}

// Creates and returns a new block argument of the specified type to the
// module. This also maintains the name attribute for the new argument,
// possibly with a new suffix appended.
std::pair<Value, PortInfo>
TypeLoweringVisitor::addArg(Operation *module, unsigned insertPt,
                            unsigned insertPtOffset, FIRRTLType srcType,
                            FlatBundleFieldEntry field, PortInfo &oldArg) {
  Value newValue;
  if (auto mod = dyn_cast<FModuleOp>(module)) {
    Block *body = mod.getBody();
    // Append the new argument.
    newValue = body->insertArgument(insertPt, field.type, oldArg.loc);
  }

  // Save the name attribute for the new argument.
  auto name = builder->getStringAttr(oldArg.name.getValue() + field.suffix);

  SmallString<16> symtmp;
  StringRef sym;
  bool oldArgHadSym = oldArg.sym && !oldArg.sym.getValue().empty();
  if (oldArgHadSym) {
    symtmp = (oldArg.sym.getValue() + field.suffix).str();
    sym = symtmp;
  } else
    sym = name.getValue();

  bool needsSym = false;
  // Populate the new arg attributes.
  auto newAnnotations =
      filterAnnotations(context, oldArg.annotations.getArrayAttr(), srcType,
                        field, needsSym, sym);
  // Flip the direction if the field is an output.
  auto direction = (Direction)((unsigned)oldArg.direction ^ field.isOutput);

  StringAttr newSym = {};
  if (needsSym || oldArgHadSym) {
    newSym = StringAttr::get(context, sym);
  }
  if (oldArgHadSym) {
    origSymbols.unionSets(oldArg.sym.getValue(), newSym.getValue());
    if (field.type.isGround()) {
      auto moduleLike = cast<FModuleLike>(module);
      auto key = origSymbols.findLeader(oldArg.sym.getValue());
      StringAttr keyAttr = StringAttr::get(moduleLike.getContext(), *key);
      assert(insertPt >= insertPtOffset + 1 && "insertPtOffset is too large");
      auto value = PortAnnoTarget(module, insertPt - 1 - insertPtOffset);
      innerRefRenames[hw::InnerRefAttr::get(moduleLike.moduleNameAttr(),
                                            keyAttr)]
          .push_back(value);
    }
  }
  return std::make_pair(newValue,
                        PortInfo{name, field.type, direction, newSym,
                                 oldArg.loc, AnnotationSet(newAnnotations)});
}

// Lower arguments with bundle type by flattening them.
bool TypeLoweringVisitor::lowerArg(FModuleLike module, size_t argIndex,
                                   size_t argsRemoved,
                                   SmallVectorImpl<PortInfo> &newArgs,
                                   SmallVectorImpl<Value> &lowering) {

  // Flatten any bundle types.
  SmallVector<FlatBundleFieldEntry> fieldTypes;
  auto srcType = newArgs[argIndex].type.cast<FIRRTLType>();
  if (!peelType(srcType, fieldTypes,
                isModuleAllowedToPreserveAggregate(module)))
    return false;

  for (const auto &field : llvm::enumerate(fieldTypes)) {
    auto newValue = addArg(module, 1 + argIndex + field.index(), argsRemoved,
                           srcType, field.value(), newArgs[argIndex]);
    newArgs.insert(newArgs.begin() + 1 + argIndex + field.index(),
                   newValue.second);
    // Lower any other arguments by copying them to keep the relative order.
    lowering.push_back(newValue.first);
  }
  return true;
}

static Value cloneAccess(ImplicitLocOpBuilder *builder, Operation *op,
                         Value rhs) {
  if (auto rop = dyn_cast<SubfieldOp>(op))
    return builder->create<SubfieldOp>(rhs, rop.fieldIndex());
  if (auto rop = dyn_cast<SubindexOp>(op))
    return builder->create<SubindexOp>(rhs, rop.index());
  if (auto rop = dyn_cast<SubaccessOp>(op))
    return builder->create<SubaccessOp>(rhs, rop.index());
  op->emitError("Unknown accessor");
  return nullptr;
}

void TypeLoweringVisitor::lowerSAWritePath(Operation *op,
                                           ArrayRef<Operation *> writePath) {
  SubaccessOp sao = cast<SubaccessOp>(writePath.back());
  auto saoType = sao.input().getType().cast<FVectorType>();
  auto selectWidth = llvm::Log2_64_Ceil(saoType.getNumElements());

  for (size_t index = 0, e = saoType.getNumElements(); index < e; ++index) {
    auto cond = builder->create<EQPrimOp>(
        sao.index(),
        builder->createOrFold<ConstantOp>(UIntType::get(context, selectWidth),
                                          APInt(selectWidth, index)));
    builder->create<WhenOp>(cond, false, [&]() {
      // Recreate the write Path
      Value leaf = builder->create<SubindexOp>(sao.input(), index);
      for (int i = writePath.size() - 2; i >= 0; --i)
        leaf = cloneAccess(builder, writePath[i], leaf);

      emitConnect(*builder, leaf, op->getOperand(1));
    });
  }
}

// Expand connects of aggregates
bool TypeLoweringVisitor::visitStmt(ConnectOp op) {
  if (processSAPath(op))
    return true;

  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // We have to expand connections even if the aggregate preservation is true.
  if (!peelType(op.dest().getType(), fields,
                /* allowedToPreserveAggregate */ false))
    return false;

  // Loop over the leaf aggregates.
  for (const auto &field : llvm::enumerate(fields)) {
    Value src = getSubWhatever(op.src(), field.index());
    Value dest = getSubWhatever(op.dest(), field.index());
    if (field.value().isOutput)
      std::swap(src, dest);
    emitConnect(*builder, dest, src);
  }
  return true;
}

// Expand connects of aggregates
bool TypeLoweringVisitor::visitStmt(StrictConnectOp op) {
  if (processSAPath(op))
    return true;

  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // We have to expand connections even if the aggregate preservation is true.
  if (!peelType(op.dest().getType(), fields,
                /* allowedToPreserveAggregate */ false))
    return false;

  // Loop over the leaf aggregates.
  for (const auto &field : llvm::enumerate(fields)) {
    Value src = getSubWhatever(op.src(), field.index());
    Value dest = getSubWhatever(op.dest(), field.index());
    if (field.value().isOutput)
      std::swap(src, dest);
    builder->create<StrictConnectOp>(dest, src);
  }
  return true;
}

bool TypeLoweringVisitor::visitStmt(WhenOp op) {
  // The WhenOp itself does not require any lowering, the only value it uses
  // is a one-bit predicate.  Recursively visit all regions so internal
  // operations are lowered.

  // Visit operations in the then block.
  lowerBlock(&op.getThenBlock());

  // Visit operations in the else block.
  if (op.hasElseRegion())
    lowerBlock(&op.getElseBlock());
  return false; // don't delete the when!
}

/// Lower memory operations. A new memory is created for every leaf
/// element in a memory's data type.
bool TypeLoweringVisitor::visitDecl(MemOp op) {
  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;

  // MemOp should have ground types so we can't preserve aggregates.
  if (!peelType(op.getDataType(), fields, false))
    return false;

  SmallVector<MemOp> newMemories;
  SmallVector<WireOp> oldPorts;

  // Wires for old ports
  for (unsigned int index = 0, end = op.getNumResults(); index < end; ++index) {
    auto result = op.getResult(index);
    auto wire = builder->create<WireOp>(
        result.getType(),
        (op.name() + "_" + op.getPortName(index).getValue()).str());
    oldPorts.push_back(wire);
    result.replaceAllUsesWith(wire.getResult());
  }
  // If annotations targeting fields of an aggregate are present, we cannot
  // flatten the memory. It must be split into one memory per aggregate field.
  // Do not overwrite the pass flag!

  // Memory for each field
  for (const auto &field : fields)
    newMemories.push_back(cloneMemWithNewType(builder, op, field));
  // Hook up the new memories to the wires the old memory was replaced with.
  for (size_t index = 0, rend = op.getNumResults(); index < rend; ++index) {
    auto result = oldPorts[index];
    auto rType = result.getType().cast<BundleType>();
    for (size_t fieldIndex = 0, fend = rType.getNumElements();
         fieldIndex != fend; ++fieldIndex) {
      auto name = rType.getElement(fieldIndex).name.getValue();
      auto oldField = builder->create<SubfieldOp>(result, fieldIndex);
      // data and mask depend on the memory type which was split.  They can also
      // go both directions, depending on the port direction.
      if (name == "data" || name == "mask" || name == "wdata" ||
          name == "wmask" || name == "rdata") {
        for (const auto &field : fields) {
          auto realOldField = getSubWhatever(oldField, field.index);
          auto newField = getSubWhatever(
              newMemories[field.index].getResult(index), fieldIndex);
          if (rType.getElement(fieldIndex).isFlip)
            std::swap(realOldField, newField);
          emitConnect(*builder, newField, realOldField);
        }
      } else {
        for (auto mem : newMemories) {
          auto newField =
              builder->create<SubfieldOp>(mem.getResult(index), fieldIndex);
          emitConnect(*builder, newField, oldField);
        }
      }
    }
  }
  return true;
}

bool TypeLoweringVisitor::visitDecl(FExtModuleOp extModule) {
  ImplicitLocOpBuilder theBuilder(extModule.getLoc(), context);
  builder = &theBuilder;

  // Top level builder
  OpBuilder builder(context);

  // Lower the module block arguments.
  SmallVector<unsigned> argsToRemove;
  auto newArgs = extModule.getPorts();
  for (size_t argIndex = 0, argsRemoved = 0; argIndex < newArgs.size();
       ++argIndex) {
    SmallVector<Value> lowering;
    if (lowerArg(extModule, argIndex, argsRemoved, newArgs, lowering)) {
      argsToRemove.push_back(argIndex);
      ++argsRemoved;
    }
    // lowerArg might have invalidated any reference to newArgs, be careful
  }

  // Remove block args that have been lowered
  for (auto ii = argsToRemove.rbegin(), ee = argsToRemove.rend(); ii != ee;
       ++ii)
    newArgs.erase(newArgs.begin() + *ii);

  SmallVector<NamedAttribute, 8> newModuleAttrs;

  // Copy over any attributes that weren't original argument attributes.
  for (auto attr : extModule->getAttrDictionary())
    // Drop old "portNames", directions, and argument attributes.  These are
    // handled differently below.
    if (attr.getName() != "portDirections" && attr.getName() != "portNames" &&
        attr.getName() != "portTypes" && attr.getName() != "portAnnotations" &&
        attr.getName() != "portSyms")
      newModuleAttrs.push_back(attr);

  SmallVector<Direction> newArgDirections;
  SmallVector<Attribute> newArgNames;
  SmallVector<Attribute, 8> newPortTypes;
  SmallVector<Attribute, 8> newArgSyms;
  SmallVector<Attribute, 8> newArgAnnotations;

  for (auto &port : newArgs) {
    newArgDirections.push_back(port.direction);
    newArgNames.push_back(port.name);
    newPortTypes.push_back(TypeAttr::get(port.type));
    newArgSyms.push_back(port.sym ? port.sym : cache.sEmpty);
    newArgAnnotations.push_back(port.annotations.getArrayAttr());
  }

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortDirections,
                     direction::packAttribute(context, newArgDirections)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortNames, builder.getArrayAttr(newArgNames)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortTypes, builder.getArrayAttr(newPortTypes)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortSyms, builder.getArrayAttr(newArgSyms)));

  newModuleAttrs.push_back(NamedAttribute(
      cache.sPortAnnotations, builder.getArrayAttr(newArgAnnotations)));

  // Update the module's attributes.
  extModule->setAttrs(newModuleAttrs);
  return false;
}

bool TypeLoweringVisitor::visitDecl(FModuleOp module) {
  auto *body = module.getBody();

  ImplicitLocOpBuilder theBuilder(module.getLoc(), context);
  builder = &theBuilder;

  // Lower the operations.
  lowerBlock(body);

  // Lower the module block arguments.
  SmallVector<unsigned> argsToRemove;
  auto newArgs = module.getPorts();
  for (size_t argIndex = 0, argsRemoved = 0; argIndex < newArgs.size();
       ++argIndex) {
    SmallVector<Value> lowerings;
    if (lowerArg(module, argIndex, argsRemoved, newArgs, lowerings)) {
      auto arg = module.getArgument(argIndex);
      processUsers(arg, lowerings);
      argsToRemove.push_back(argIndex);
      ++argsRemoved;
    }
    // lowerArg might have invalidated any reference to newArgs, be careful
  }

  // Remove block args that have been lowered.
  body->eraseArguments(argsToRemove);
  for (auto deadArg : llvm::reverse(argsToRemove))
    newArgs.erase(newArgs.begin() + deadArg);

  SmallVector<NamedAttribute, 8> newModuleAttrs;

  // Copy over any attributes that weren't original argument attributes.
  for (auto attr : module->getAttrDictionary())
    // Drop old "portNames", directions, and argument attributes.  These are
    // handled differently below.
    if (attr.getName() != "portNames" && attr.getName() != "portDirections" &&
        attr.getName() != "portTypes" && attr.getName() != "portAnnotations" &&
        attr.getName() != "portSyms")
      newModuleAttrs.push_back(attr);

  SmallVector<Direction> newArgDirections;
  SmallVector<Attribute> newArgNames;
  SmallVector<Attribute> newArgTypes;
  SmallVector<Attribute> newArgSyms;
  SmallVector<Attribute, 8> newArgAnnotations;
  for (auto &port : newArgs) {
    newArgDirections.push_back(port.direction);
    newArgNames.push_back(port.name);
    newArgTypes.push_back(TypeAttr::get(port.type));
    newArgSyms.push_back(port.sym ? port.sym : cache.sEmpty);
    newArgAnnotations.push_back(port.annotations.getArrayAttr());
  }

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortDirections,
                     direction::packAttribute(context, newArgDirections)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortNames, builder->getArrayAttr(newArgNames)));

  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortTypes, builder->getArrayAttr(newArgTypes)));
  newModuleAttrs.push_back(
      NamedAttribute(cache.sPortSyms, builder->getArrayAttr(newArgSyms)));
  newModuleAttrs.push_back(NamedAttribute(
      cache.sPortAnnotations, builder->getArrayAttr(newArgAnnotations)));

  // Update the module's attributes.
  module->setAttrs(newModuleAttrs);
  return false;
}

/// Lower a wire op with a bundle to multiple non-bundled wires.
bool TypeLoweringVisitor::visitDecl(WireOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    return builder->create<WireOp>(field.type, "", NameKindEnum::DroppableName,
                                   attrs, StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower a reg op with a bundle to multiple non-bundled regs.
bool TypeLoweringVisitor::visitDecl(RegOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    return builder->create<RegOp>(field.type, op.clockVal(), "",
                                  NameKindEnum::DroppableName, attrs,
                                  StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower a reg op with a bundle to multiple non-bundled regs.
bool TypeLoweringVisitor::visitDecl(RegResetOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    auto resetVal = getSubWhatever(op.resetValue(), field.index);
    return builder->create<RegResetOp>(
        field.type, op.clockVal(), op.resetSignal(), resetVal, "",
        NameKindEnum::DroppableName, attrs, StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower a wire op with a bundle to multiple non-bundled wires.
bool TypeLoweringVisitor::visitDecl(NodeOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    auto input = getSubWhatever(op.input(), field.index);
    return builder->create<NodeOp>(field.type, input, "",
                                   NameKindEnum::DroppableName, attrs,
                                   StringAttr{});
  };
  return lowerProducer(op, clone);
}

/// Lower an InvalidValue op with a bundle to multiple non-bundled InvalidOps.
bool TypeLoweringVisitor::visitExpr(InvalidValueOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    return builder->create<InvalidValueOp>(field.type);
  };
  return lowerProducer(op, clone);
}

// Expand muxes of aggregates
bool TypeLoweringVisitor::visitExpr(MuxPrimOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    auto high = getSubWhatever(op.high(), field.index);
    auto low = getSubWhatever(op.low(), field.index);
    return builder->create<MuxPrimOp>(op.sel(), high, low);
  };
  return lowerProducer(op, clone);
}

// Expand UnrealizedConversionCastOp of aggregates
bool TypeLoweringVisitor::visitExpr(mlir::UnrealizedConversionCastOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    auto input = getSubWhatever(op.getOperand(0), field.index);
    return builder->create<mlir::UnrealizedConversionCastOp>(field.type, input);
  };
  return lowerProducer(op, clone);
}

// Expand BitCastOp of aggregates
bool TypeLoweringVisitor::visitExpr(BitCastOp op) {
  Value srcLoweredVal = op.input();
  // If the input is of aggregate type, then cat all the leaf fields to form a
  // UInt type result. That is, first bitcast the aggregate type to a UInt.
  // Attempt to get the bundle types.
  SmallVector<FlatBundleFieldEntry> fields;
  if (peelType(op.input().getType(), fields,
               /* allowedToPreserveAggregate */ false)) {
    size_t uptoBits = 0;
    // Loop over the leaf aggregates and concat each of them to get a UInt.
    // Bitcast the fields to handle nested aggregate types.
    for (const auto &field : llvm::enumerate(fields)) {
      auto fieldBitwidth = getBitWidth(field.value().type).getValue();
      // Ignore zero width fields, like empty bundles.
      if (fieldBitwidth == 0)
        continue;
      Value src = getSubWhatever(op.input(), field.index());
      // The src could be an aggregate type, bitcast it to a UInt type.
      src = builder->createOrFold<BitCastOp>(
          UIntType::get(context, fieldBitwidth), src);
      // Take the first field, or else Cat the previous fields with this field.
      if (uptoBits == 0)
        srcLoweredVal = src;
      else
        srcLoweredVal = builder->create<CatPrimOp>(src, srcLoweredVal);
      // Record the total bits already accumulated.
      uptoBits += fieldBitwidth;
    }
  } else {
    srcLoweredVal = builder->createOrFold<AsUIntPrimOp>(srcLoweredVal);
  }
  // Now the input has been cast to srcLoweredVal, which is of UInt type.
  // If the result is an aggregate type, then use lowerProducer.
  if (op.getResult().getType().isa<BundleType, FVectorType>()) {
    // uptoBits is used to keep track of the bits that have been extracted.
    size_t uptoBits = 0;
    auto clone = [&](const FlatBundleFieldEntry &field,
                     ArrayAttr attrs) -> Operation * {
      // All the fields must have valid bitwidth, a requirement for BitCastOp.
      auto fieldBits = getBitWidth(field.type).getValue();
      // If empty field, then it doesnot have any use, so replace it with an
      // invalid op, which should be trivially removed.
      if (fieldBits == 0)
        return builder->create<InvalidValueOp>(field.type);

      // Assign the field to the corresponding bits from the input.
      // Bitcast the field, incase its an aggregate type.
      auto extractBits = builder->create<BitsPrimOp>(
          srcLoweredVal, uptoBits + fieldBits - 1, uptoBits);
      uptoBits += fieldBits;
      return builder->create<BitCastOp>(field.type, extractBits);
    };
    return lowerProducer(op, clone);
  }

  // If ground type, then replace the result.
  if (op.getType().dyn_cast<SIntType>())
    srcLoweredVal = builder->create<AsSIntPrimOp>(srcLoweredVal);
  op.getResult().replaceAllUsesWith(srcLoweredVal);
  return true;
}

bool TypeLoweringVisitor::visitDecl(InstanceOp op) {
  bool skip = true;
  SmallVector<Type, 8> resultTypes;
  SmallVector<int64_t, 8> endFields; // Compressed sparse row encoding
  auto oldPortAnno = op.portAnnotations();
  SmallVector<Direction> newDirs;
  SmallVector<Attribute> newNames;
  SmallVector<Attribute> newPortAnno;
  bool allowedToPreserveAggregate =
      isModuleAllowedToPreserveAggregate(op.getReferencedModule(symTbl));

  endFields.push_back(0);
  bool needsSymbol = false;
  for (size_t i = 0, e = op.getNumResults(); i != e; ++i) {
    auto srcType = op.getType(i).cast<FIRRTLType>();

    // Flatten any nested bundle types the usual way.
    SmallVector<FlatBundleFieldEntry, 8> fieldTypes;
    if (!peelType(srcType, fieldTypes, allowedToPreserveAggregate)) {
      newDirs.push_back(op.getPortDirection(i));
      newNames.push_back(op.getPortName(i));
      resultTypes.push_back(srcType);
      newPortAnno.push_back(oldPortAnno[i]);
    } else {
      skip = false;
      auto oldName = op.getPortNameStr(i);
      auto oldDir = op.getPortDirection(i);
      // Store the flat type for the new bundle type.
      for (const auto &field : fieldTypes) {
        newDirs.push_back(direction::get((unsigned)oldDir ^ field.isOutput));
        newNames.push_back(builder->getStringAttr(oldName + field.suffix));
        resultTypes.push_back(field.type);
        auto annos = filterAnnotations(
            context, oldPortAnno[i].dyn_cast_or_null<ArrayAttr>(), srcType,
            field, needsSymbol, "");
        newPortAnno.push_back(annos);
      }
    }
    endFields.push_back(resultTypes.size());
  }

  auto sym = getInnerSymName(op);

  if (skip) {
    return false;
  }
  if (!sym || sym.getValue().empty())
    if (needsSymbol)
      sym = StringAttr::get(builder->getContext(),
                            "sym" + op.nameAttr().getValue());
  // FIXME: annotation update
  auto newInstance = builder->create<InstanceOp>(
      resultTypes, op.moduleNameAttr(), op.nameAttr(), op.nameKindAttr(),
      direction::packAttribute(context, newDirs),
      builder->getArrayAttr(newNames), op.annotations(),
      builder->getArrayAttr(newPortAnno), op.lowerToBindAttr(),
      sym ? InnerSymAttr::get(sym) : InnerSymAttr());

  SmallVector<Value> lowered;
  for (size_t aggIndex = 0, eAgg = op.getNumResults(); aggIndex != eAgg;
       ++aggIndex) {
    lowered.clear();
    for (size_t fieldIndex = endFields[aggIndex],
                eField = endFields[aggIndex + 1];
         fieldIndex < eField; ++fieldIndex)
      lowered.push_back(newInstance.getResult(fieldIndex));
    if (lowered.size() != 1 ||
        op.getType(aggIndex) != resultTypes[endFields[aggIndex]])
      processUsers(op.getResult(aggIndex), lowered);
    else
      op.getResult(aggIndex).replaceAllUsesWith(lowered[0]);
  }
  return true;
}

bool TypeLoweringVisitor::visitExpr(SubaccessOp op) {
  auto input = op.input();
  auto vType = input.getType().cast<FVectorType>();

  // Check for empty vectors
  if (vType.getNumElements() == 0) {
    Value inv = builder->create<InvalidValueOp>(vType.getElementType());
    op.replaceAllUsesWith(inv);
    return true;
  }

  // Check for constant instances
  if (ConstantOp arg =
          dyn_cast_or_null<ConstantOp>(op.index().getDefiningOp())) {
    auto sio =
        builder->create<SubindexOp>(op.input(), arg.value().getExtValue());
    op.replaceAllUsesWith(sio.getResult());
    return true;
  }

  // Construct a multibit mux
  SmallVector<Value> inputs;
  inputs.reserve(vType.getNumElements());
  for (int index = vType.getNumElements() - 1; index >= 0; index--)
    inputs.push_back(builder->create<SubindexOp>(input, index));

  Value multibitMux = builder->create<MultibitMuxOp>(op.index(), inputs);
  op.replaceAllUsesWith(multibitMux);
  return true;
}

bool TypeLoweringVisitor::visitExpr(MultibitMuxOp op) {
  auto clone = [&](const FlatBundleFieldEntry &field,
                   ArrayAttr attrs) -> Operation * {
    SmallVector<Value> newInputs;
    newInputs.reserve(op.inputs().size());
    for (auto input : op.inputs()) {
      auto inputSub = getSubWhatever(input, field.index);
      newInputs.push_back(inputSub);
    }
    return builder->create<MultibitMuxOp>(op.index(), newInputs);
  };
  return lowerProducer(op, clone);
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct LowerTypesPass : public LowerFIRRTLTypesBase<LowerTypesPass> {
  LowerTypesPass(bool preserveAggregateFlag, bool preservePublicTypesFlag) {
    preserveAggregate = preserveAggregateFlag;
    preservePublicTypes = preservePublicTypesFlag;
  }
  void runOnOperation() override;
};
} // end anonymous namespace

// This is the main entrypoint for the lowering pass.
void LowerTypesPass::runOnOperation() {
  LLVM_DEBUG(
      llvm::dbgs() << "===- Running LowerTypes Pass "
                      "------------------------------------------------===\n");
  std::vector<FModuleLike> ops;
  // Symbol Table
  SymbolTable symTbl(getOperation());
  // Cached attr
  AttrCache cache(&getContext());

  // Record all operations in the circuit.
  llvm::for_each(getOperation().getBody()->getOperations(), [&](Operation &op) {
    // Creating a map of all ops in the circt, but only modules are relevant.
    if (auto module = dyn_cast<FModuleLike>(op))
      ops.push_back(module);
  });
  auto *nlaTable = &getAnalysis<NLATable>();

  LLVM_DEBUG(llvm::dbgs() << "Recording Inner Symbol Renames:\n");

  // Lower each module and return a list of Nlas which need to be updated with
  // the new symbol names.
  DenseMap<hw::InnerRefAttr, SmallVector<AnnoTarget>> innerRefRenames;
  std::mutex nlaAppendLock;
  // This lambda, executes in parallel for each Op within the circt.
  auto lowerModules = [&](FModuleLike op) -> void {
    auto tl = TypeLoweringVisitor(&getContext(), preserveAggregate,
                                  preservePublicTypes, symTbl, cache);
    tl.lowerModule(op);

    std::lock_guard<std::mutex> lg(nlaAppendLock);
    // This section updates shared data structures using a lock.
    for (const auto &keyValue : tl.getRenames()) {
      innerRefRenames.insert(keyValue);
    }

    LLVM_DEBUG({
      auto &renames = tl.getRenames();
      if (!renames.empty())
        llvm::dbgs() << "  - Module: @" << op.moduleName() << "\n";
      for (auto keyValue : renames) {
        llvm::dbgs() << "    - @" << keyValue.first.getName().getValue()
                     << ": [";
        llvm::interleaveComma(
            keyValue.second, llvm::dbgs(), [&](AnnoTarget target) {
              if (auto port = target.dyn_cast<PortAnnoTarget>()) {
                auto module = cast<FModuleLike>(port.getOp());
                llvm::dbgs() << FlatSymbolRefAttr::get(
                    module.getContext(), module.getPortName(port.getPortNo()));
              } else
                llvm::dbgs() << FlatSymbolRefAttr::get(
                    target.getOp()->getAttrOfType<StringAttr>("name"));
            });
        llvm::dbgs() << "]\n";
      }
    });
  };
  parallelForEach(&getContext(), ops.begin(), ops.end(), lowerModules);

  // Update all the hierarchical paths based on the innerRefRenames map.
  // Iterate over each InnerRefAttr that was updated.  Replace any hierarchical
  // paths that end in this InnerRefAttr with all values in the innerRefRenames
  // map.
  LLVM_DEBUG(llvm::dbgs() << "Updating hierarhical paths:\n");
  CircuitNamespace circtNamespace(getOperation());
  for (auto pair : innerRefRenames) {
    auto [oldRef, newRefs] = pair;
    // Lookup all NLAs which participate in the module of the old InnerRefAttr,
    // but only visit ones which end in this old InnerRefAttr.
    //
    // TODO: A utility on the NLATable for this query would refactor this.
    ArrayRef<HierPathOp> foo = nlaTable->lookup(oldRef.getModule());
    for (auto path : SmallVector<HierPathOp>(foo.begin(), foo.end())) {
      // Skip this hierarchical path if it targets the wrong InnerRefAttr.
      // (This also covers the case of not visiting any NLAs which end at
      // modules and do not target something inside the module.)
      if (oldRef != path.namepath().getValue().back())
        continue;

      // Split the old hierarchical path into one hierarchical path for each new
      // InnerRefAttr.  Update the symbols in any NLAs which use the old
      // InnerRefAttr to the correct new InnerRefAttr.
      auto namepath = path.namepath().getValue();
      // Grab the old namepath.  We reuse all but the last element of this.
      SmallVector<Attribute> newNamepath{namepath.begin(), namepath.end()};
      ImplicitLocOpBuilder builder(path.getLoc(), path);
      builder.setInsertionPointAfter(path);
      StringAttr oldSym;
      assert(!newRefs.empty() && "LowerTypes should not delete InnerRefAttrs");
      for (auto &target : newRefs) {
        // Drop the last part of the namepath so we can replace it.
        newNamepath.pop_back();

        // Re-use the old hierarchical path symbol for the first new
        // hierarchical path.  Generate a new symbol for any later paths.
        StringAttr newSym;
        if (!oldSym) {
          oldSym = path.getNameAttr();
          newSym = oldSym;
          // Delete the old hierarchical path from the NLA and symbol tables.
          nlaTable->erase(path, &symTbl);
        } else
          newSym =
              builder.getStringAttr(circtNamespace.newName(oldSym.getValue()));

        // This is the new annotation sequence.  Put the update method into a
        // lambda to enable reuse for operation and port annotations.
        SmallVector<Annotation> newAnnotations;
        auto updateNLASymbol = [&](Annotation anno) -> bool {
          auto sym = anno.getMember<FlatSymbolRefAttr>("circt.nonlocal");
          if (!sym || sym.getAttr() != oldSym)
            return false;
          anno.setMember("circt.nonlocal", FlatSymbolRefAttr::get(newSym));
          newAnnotations.push_back(anno);
          return true;
        };

        // Update annotations the operation or on the port.
        HierPathOp newPath;
        TypeSwitch<AnnoTarget>(target)
            .Case<OpAnnoTarget>([&](OpAnnoTarget target) {
              auto *op = target.getOp();
              newNamepath.push_back(hw::InnerRefAttr::get(
                  target.getModule().moduleNameAttr(), getInnerSymName(op)));
              newPath = builder.create<HierPathOp>(
                  newSym, builder.getArrayAttr(newNamepath));
              AnnotationSet annotations(op);
              annotations.removeAnnotations(updateNLASymbol);
              annotations.addAnnotations(newAnnotations);
              annotations.applyToOperation(op);
            })
            .Case<PortAnnoTarget>([&](PortAnnoTarget target) {
              auto op = cast<FModuleLike>(target.getOp());
              auto portIdx = target.getPortNo();
              newNamepath.push_back(
                  hw::InnerRefAttr::get(target.getModule().moduleNameAttr(),
                                        op.getPortSymbolAttr(portIdx)));
              newPath = builder.create<HierPathOp>(
                  newSym, builder.getArrayAttr(newNamepath));
              auto annotations = AnnotationSet::forPort(op, portIdx);
              annotations.removeAnnotations(updateNLASymbol);
              annotations.addAnnotations(newAnnotations);
              annotations.applyToPort(op, portIdx);
            })
            .Default([](auto) {
              llvm_unreachable("match on unkonwn AnnoTarget type");
            });

        // Add the new hierarchical path to the NLA Table and Symbol Table.
        nlaTable->addNLA(newPath);
        symTbl.insert(newPath);
      }
    }
  }
}

/// This is the pass constructor.
std::unique_ptr<mlir::Pass>
circt::firrtl::createLowerFIRRTLTypesPass(bool preserveAggregate,
                                          bool preservePublicTypes) {

  return std::make_unique<LowerTypesPass>(preserveAggregate,
                                          preservePublicTypes);
}
