//===- MSFTOps.td - MSFT operations definitions ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "circt/Dialect/HW/HWOpInterfaces.td"

def InstanceOp : MSFTOp<"instance", [
        Symbol,
        ParentOneOf<["MSFTModuleOp"]>,
        DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]>,
        DeclareOpInterfaceMethods<SymbolUserOpInterface>,
        DeclareOpInterfaceMethods<HWInstanceLike>
    ]> {
  let summary = "Instantiate a module";

  let arguments = (ins SymbolNameAttr:$sym_name,
                       FlatSymbolRefAttr:$moduleName,
                       Variadic<AnyType>:$inputs,
                       OptionalAttr<ParamDeclArrayAttr>:$parameters,
                       OptionalAttr<SymbolRefAttr>:$targetDesignPartition);
  let results = (outs Variadic<AnyType>);

  let builders = [
    OpBuilder<(ins "ArrayRef<Type>":$resultTypes, "StringAttr":$sym_name,
                   "FlatSymbolRefAttr":$moduleName, "ArrayRef<Value>":$inputs)>
  ];

  let extraClassDeclaration = [{
    // Return the name of the specified result or empty string if it cannot be
    // determined.
    StringAttr getResultName(size_t i);

    /// Lookup the module or extmodule for the symbol.  This returns null on
    /// invalid IR.
    Operation *getReferencedModule();

    /// Instance name is the same as the symbol name. This may change in the
    /// future.
    StringRef instanceName() {
      return sym_name();
    }
    StringAttr instanceNameAttr() {
      return sym_nameAttr();
    }
    /// Check that the operands and results match the module specified.
    LogicalResult verifySignatureMatch(const circt::hw::ModulePortInfo&);

    // Update the results.
    InstanceOp getWithNewResults(MSFTModuleOp mod,
                                 ArrayRef<unsigned> newToOldMap);
  }];

  /// sym keyword for optional symbol simplifies parsing
  let assemblyFormat = [{
    $sym_name $moduleName `(` $inputs `)` custom<ParameterList>($parameters)
      attr-dict `:` functional-type($inputs, results)
  }];
}

def OneOrNoBlocksRegion : Region<
  CPred<"::llvm::hasNItemsOrLess($_self, 1)">,
  "region with at most 1 block">;

def AppIDArrayAttr : TypedArrayAttrBase<AppIDAttr, "Array of AppIDs">;

def MSFTModuleOp : MSFTOp<"module",
      [IsolatedFromAbove, FunctionOpInterface, Symbol, RegionKindInterface,
       HasParent<"mlir::ModuleOp">,
       SingleBlockImplicitTerminator<"OutputOp">,
       OpAsmOpInterface,
       DeclareOpInterfaceMethods<HWModuleLike>]>{
  let summary = "MSFT HW Module";
  let description = [{
    A lot like `hw.module`, but with a few differences:
      - Can exist without a body. The body is filled in by a generator post op
      creation.
      - Provides methods for mutation.
  }];
  let arguments = (ins
      StrArrayAttr:$argNames, StrArrayAttr:$resultNames,
      DictionaryAttr:$parameters,
      OptionalAttr<StrAttr>:$fileName,
      OptionalAttr<StrArrayAttr>:$childAppIDBases);
  let results = (outs);
  let regions = (region OneOrNoBlocksRegion:$body);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "StringAttr":$name, "hw::ModulePortInfo":$ports,
                   "ArrayRef<NamedAttribute>":$params)>
  ];

  let extraClassDeclaration = [{
    using mlir::detail::FunctionOpInterfaceTrait<MSFTModuleOp>::front;
    using mlir::detail::FunctionOpInterfaceTrait<MSFTModuleOp>::getBody;

    // Implement RegionKindInterface.
    static RegionKind getRegionKind(unsigned index) {
      return RegionKind::Graph;
    }

    // Decode information about the input and output ports on this module.
    ::circt::hw::ModulePortInfo getPorts();

    // Adds input and output ports. Returns a list of new block arguments for
    // the new inputs.
    SmallVector<BlockArgument> addPorts(
      ArrayRef<std::pair<StringAttr, Type>> inputs,
      ArrayRef<std::pair<StringAttr, Value>> outputs);

    // Remove the ports at the specified indexes. Returns the new to old result
    // mapping.
    SmallVector<unsigned>
    removePorts(llvm::BitVector inputs, llvm::BitVector outputs);

    // Get the module's symbolic name as StringAttr.
    StringAttr getNameAttr() {
      return (*this)->getAttrOfType<StringAttr>(
        ::mlir::SymbolTable::getSymbolAttrName());
    }

    // Get the module's symbolic name.
    StringRef getName() {
      return getNameAttr().getValue();
    }

    Block *getBodyBlock() { return &body().front(); }
    void getAsmBlockArgumentNames(mlir::Region &region,
                                  mlir::OpAsmSetValueNameFn setNameFn);

    /// Returns the type of this function.
    FunctionType getFunctionType() {
      return getFunctionTypeAttr().getValue().cast<FunctionType>();
    }

    /// Returns the argument types of this function.
    ArrayRef<Type> getArgumentTypes() { return getFunctionType().getInputs(); }

    /// Returns the result types of this function.
    ArrayRef<Type> getResultTypes() { return getFunctionType().getResults(); }

    /// Verify the type attribute of this function. Returns failure and emits
    /// an error if the attribute is invalid.
    LogicalResult verifyType() {
      auto type = getFunctionTypeAttr().getValue();
      if (!type.isa<FunctionType>())
        return emitOpError(
            "requires '" + mlir::function_interface_impl::getTypeAttrName() +
            "' attribute of function type");
      return success();
    }
  }];

  let hasCustomAssemblyFormat = 1;
}

def MSFTModuleExternOp : MSFTOp<"module.extern",
      [Symbol, HasParent<"mlir::ModuleOp">]> {
  let summary = "MSFT external Module";
  let description = [{
    Identical to `hw.module.extern`, and trivially lowers to that. This op
    exists so that we can use `msft.instance` to refer to both `msft.module` and
    `msft.module.extern`, rather than mixing `hw.instance` with `msft.instance`.
  }];
  let arguments = (ins StrArrayAttr:$argNames, StrArrayAttr:$resultNames,
                       ParamDeclArrayAttr:$parameters,
                       OptionalAttr<StrAttr>:$verilogName);
  let regions = (region SizedRegion<0>:$body);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins "StringAttr":$name, "const hw::ModulePortInfo &":$ports,
                   CArg<"StringRef", "StringRef()">:$verilogName,
                   CArg<"ArrayAttr", "{}">:$parameters,
                   CArg<"ArrayRef<NamedAttribute>", "{}">:$attributes)>
  ];

  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;

  let extraClassDeclaration = [{
    /// Decode information about the input and output ports on this module.
    hw::ModulePortInfo getPorts();
  }];
}

def DesignPartitionOp : MSFTOp<"partition",
                               [Symbol, HasParent<"msft::MSFTModuleOp">]> {
  let summary = "A target \"module\" for moving entities";
  let description = [{
    Sometimes EDA tools require designs to have a module hierarchy which doesn't
    match the logical structure a designer would like to have. "Design
    partitions" allow the designer to "tag" entities (instances, registers, etc.)
    with a target design partition. During lowering, CIRCT will modify the
    hierarchy to move the tagged entities into the design partition module. The
    target design partition can then be used by subsequent EDA tools.
  }];

  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$verilogName);
  let assemblyFormat = "$sym_name `,` $verilogName attr-dict";
}

def OutputOp : MSFTOp<"output", [Terminator, HasParent<"MSFTModuleOp">,
                                NoSideEffect, ReturnLike]> {
  let summary = "termination operation";

  let arguments = (ins Variadic<AnyType>:$operands);
  let builders = [
    OpBuilder<(ins)>
  ];

  let assemblyFormat = "attr-dict ($operands^ `:` qualified(type($operands)))?";
}
