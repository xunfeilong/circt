//===- Seq.td - Seq dialect definition ---------------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the top level file for the Seq dialect. It contains the one op and
// pass. Once we add more than one, we should break it out like the other
// dialects.
//
//===----------------------------------------------------------------------===//

#ifndef SEQ_TD
#define SEQ_TD

include "mlir/IR/OpBase.td"
include "mlir/IR/OpAsmInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Pass/PassBase.td"

def SeqDialect : Dialect {
  let name = "seq";

  let summary = "Types and operations for seq dialect";
  let description = [{
    The `seq` dialect is intended to model digital sequential logic.
  }];

  let hasConstantMaterializer = 1;
  let cppNamespace = "::circt::seq";
}

// Base class for the operation in this dialect.
class SeqOp<string mnemonic, list<Trait> traits = []> :
    Op<SeqDialect, mnemonic, traits>;

def CompRegOp : SeqOp<"compreg",
    [NoSideEffect, AllTypesMatch<["input", "data"/*, "resetValue"*/]>,
     SameVariadicOperandSize,
     DeclareOpInterfaceMethods<OpAsmOpInterface, ["getAsmResultNames"]> ]> {
       // AllTypesMatch doesn't work with Optional types yet.

  let summary = "Register a value, storing it for one cycle";
  let description = "See the Seq dialect rationale for a longer description";

  let arguments = (ins AnyType:$input, I1:$clk, StrAttr:$name,
    Optional<I1>:$reset, Optional<AnyType>:$resetValue,
    OptionalAttr<SymbolNameAttr>:$sym_name);
  let results = (outs AnyType:$data);
  let hasCustomAssemblyFormat = 1;

  let builders = [
    OpBuilder<(ins "Value":$input, "Value":$clk, "StringRef":$sym_name), [{
      return build($_builder, $_state, input.getType(),
                   input, clk, sym_name, Value(), Value(),
                   StringAttr::get($_builder.getContext(), sym_name));
    }]>,
  ];
}

#endif // SEQ_TD
