/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/backends/gpu/codegen/triton/ir/triton_xla_ops.h"

#include <cassert>
#include <optional>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TypeSwitch.h"  // IWYU pragma: keep
#include "llvm/Support/Casting.h"
#include "llvm/Support/LogicalResult.h"
#include "mlir/IR/Builders.h"  // IWYU pragma: keep
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/DialectImplementation.h"  // IWYU pragma: keep
#include "mlir/IR/MLIRContext.h"  // IWYU pragma: keep
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"  // IWYU pragma: keep
#include "mlir/IR/Region.h"
#include "mlir/IR/TypeUtilities.h"  // IWYU pragma: keep
#include "mlir/IR/ValueRange.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "xla/backends/gpu/codegen/triton/ir/triton_xla_dialect.cc.inc"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/Types.h"

using mlir::Dialect;
using mlir::DictionaryAttr;
using mlir::Location;
using mlir::LogicalResult;
using mlir::MLIRContext;
using mlir::OpaqueProperties;
using mlir::RankedTensorType;
using mlir::RegionRange;
using mlir::SmallVectorImpl;
using mlir::Type;
using mlir::ValueRange;
using mlir::triton::gpu::TensorOrMemDesc;

namespace mlir::triton::xla {

//===----------------------------------------------------------------------===//
// SparseDotOp
//===----------------------------------------------------------------------===//

LogicalResult SparseDotOp::inferReturnTypes(
    MLIRContext* context, std::optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, OpaqueProperties properties, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  // DotOp::inferReturnTypes() no longer handles MemDescType, so we need to
  // handle it ourselves.
  // TODO: b/382459490 - Remove the need for our own implementation once we've
  // cleaned up the sparsity extension.

  // type is the same as the accumulator
  auto accTy = cast<RankedTensorType>(operands[2].getType());
  inferredReturnTypes.push_back(accTy);

  // verify encodings
  auto aEnc = cast<TensorOrMemDesc>(operands[0].getType()).getEncoding();
  auto bEnc = cast<TensorOrMemDesc>(operands[1].getType()).getEncoding();
  auto retEnc = accTy.getEncoding();
  if (aEnc) {
    assert(bEnc && retEnc);
    Dialect& dialect = retEnc.getDialect();
    auto interface = dyn_cast<DialectInferLayoutInterface>(&dialect);
    if (interface->inferDotOpEncoding(aEnc, 0, retEnc, location).failed())
      return failure();
    if (interface->inferDotOpEncoding(bEnc, 1, retEnc, location).failed())
      return failure();
  }
  return success();
}

LogicalResult SparseDotOp::verify() {
  // Implied properties of 2:4 sparse dots.
  constexpr int kContractingFactor = 2;
  constexpr int kMetadataElementsPerPackedValue = 8;
  // Verify operand A.
  auto aTensorTy = llvm::cast<TensorOrMemDesc>(getOperand(0).getType());
  auto aElemTy = aTensorTy.getElementType();
  if (!aElemTy.isF16() && !aElemTy.isBF16())
    return emitError("element type of operand A is not supported");
  auto aShape = aTensorTy.getShape();
  if (aShape.size() != 2) return emitError("shape of operand A is incorrect");

  // Verify operand B.
  auto bTensorTy = llvm::cast<TensorOrMemDesc>(getOperand(1).getType());
  auto bElemTy = bTensorTy.getElementType();
  if (!bElemTy.isF16() && !bElemTy.isBF16())
    return emitError("element type of operand B is not supported");
  auto bShape = bTensorTy.getShape();
  if (bShape.size() != 2) return emitError("shape of operand B is incorrect");

  // Verify operand C.
  auto cTensorTy = llvm::cast<RankedTensorType>(getOperand(2).getType());
  auto cElemTy = cTensorTy.getElementType();
  if (!cElemTy.isF32())
    return emitError("element type of operand C is not supported");
  auto cShape = cTensorTy.getShape();
  if (cShape.size() != 2) return emitError("shape of operand C is incorrect");

  // Check operand dependencies.
  if (aShape[0] != cShape[0] || bShape[1] != cShape[1] ||
      bShape[0] != aShape[1] * kContractingFactor)
    return emitError("operand shape dimensions are incorrect");
  if (aElemTy != bElemTy)
    return emitError("operand element types do not match");

  // Verify sparse metadata.
  auto metaTy = llvm::cast<RankedTensorType>(getOperand(3).getType());
  auto metaShape = metaTy.getShape();
  if (!metaTy.getElementType().isInteger(16) || metaShape.size() != 2)
    return emitError("sparse metadata tensor is invalid");
  if (metaShape[0] != aShape[0] ||
      metaShape[1] * kMetadataElementsPerPackedValue != aShape[1])
    return emitError("sparse metadata shape dimensions are incorrect");

  // Verify tensor encoding.
  auto aEncoding = aTensorTy.getEncoding();
  auto bEncoding = bTensorTy.getEncoding();
  if (!aEncoding && !bEncoding) return mlir::success();
  if (!aEncoding || !bEncoding)
    return emitError("mismatching encoding between A and B operands");

  Dialect& dialect = aEncoding.getDialect();
  auto interface = llvm::cast<DialectInferLayoutInterface>(&dialect);
  return interface->verifyDotOpEncodingCompatibility(getOperation(), aEncoding,
                                                     bEncoding);
}

//===----------------------------------------------------------------------===//
// TileOp
//===----------------------------------------------------------------------===//

void TileOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "tiled_tensor");
}

template <typename DenseIntArrayAttrType>
mlir::ParseResult parseDenseIntArrayAttr(mlir::AsmParser& parser,
                                         DenseIntArrayAttrType& array) {
  array = mlir::dyn_cast_or_null<DenseIntArrayAttrType>(
      DenseIntArrayAttrType::parse(parser, mlir::Type{}));
  if (!array) return mlir::failure();
  return mlir::success();
}

ParseResult TileOp::parse(OpAsmParser& parser, OperationState& result) {
  OpAsmParser::UnresolvedOperand src;
  TiledTensorType tiled_tensor_type;
  DenseI64ArrayAttr strides;
  DenseI32ArrayAttr offsets, sizes;
  if (parser.parseOperand(src) || parseDenseIntArrayAttr(parser, offsets) ||
      parseDenseIntArrayAttr(parser, sizes) ||
      parseDenseIntArrayAttr(parser, strides) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(tiled_tensor_type)) {
    return failure();
  }
  if (parser.resolveOperand(src, tiled_tensor_type.getOriginalType(),
                            result.operands)) {
    return failure();
  }
  result.addAttribute("offsets", offsets);
  result.addAttribute("sizes", sizes);
  result.addAttribute("strides", strides);
  result.addTypes(tiled_tensor_type);
  return success();
}

void TileOp::print(OpAsmPrinter& p) {
  p << ' ' << getTensor();
  p << '[';
  llvm::interleaveComma(getOffsets(), p);
  p << "][";
  llvm::interleaveComma(getSizes(), p);
  p << "][";
  llvm::interleaveComma(getStrides(), p);
  p << "] : " << getType();
}

LogicalResult TileOp::verify() {
  if (getTensor().getType().getRank() == 0) {
    return emitError("cannot tile a 0-d tensor");
  }
  auto tensor_rank = getTensor().getType().getRank();
  if (tensor_rank != getOffsets().size() || tensor_rank != getSizes().size() ||
      tensor_rank != getStrides().size())
    return emitError(
        "mismatch between tensor rank and one or more of "
        "offsets/sizes/strides");
  return success();
}

//===----------------------------------------------------------------------===//
// ExtractOp
//===----------------------------------------------------------------------===//

void ExtractOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "extracted_tile");
}

ParseResult ExtractOp::parse(OpAsmParser& parser, OperationState& result) {
  Builder& builder = parser.getBuilder();

  OpAsmParser::UnresolvedOperand tiled_tensor;
  Type tile_type, original_type;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> offsets;
  if (parser.parseOperand(tiled_tensor) ||
      parser.parseOperandList(offsets, OpAsmParser::Delimiter::Square) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(original_type) || parser.parseKeyword("to") ||
      parser.parseType(tile_type)) {
    return failure();
  }
  auto tiled_tensor_type = TiledTensorType::get(
      parser.getContext(), mlir::cast<RankedTensorType>(tile_type),
      mlir::cast<RankedTensorType>(original_type));
  auto offset_type = builder.getI32Type();
  if (parser.resolveOperand(tiled_tensor, tiled_tensor_type, result.operands) ||
      parser.resolveOperands(offsets, offset_type, result.operands)) {
    return failure();
  }
  result.addTypes(tile_type);
  return success();
}

void ExtractOp::print(OpAsmPrinter& p) {
  TiledTensorType tiled_type = getSrc().getType();
  p << ' ' << getSrc() << '[';
  llvm::interleaveComma(getOffsets(), p);
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs());
  p << " : " << tiled_type.getOriginalType() << " to "
    << tiled_type.getTileType();
}

LogicalResult ExtractOp::verify() {
  if (getResult().getType().getRank() == 0) {
    return emitError("cannot extract a 0-d tensor");
  }
  if (getSrc().getType().getRank() != getOffsets().size())
    return emitError("source tensor rank does not match number of offsets");
  return success();
}

//===----------------------------------------------------------------------===//
// InsertOp
//===----------------------------------------------------------------------===//

void InsertOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "inserted_tile");
}

ParseResult InsertOp::parse(OpAsmParser& parser, OperationState& result) {
  Builder& builder = parser.getBuilder();

  OpAsmParser::UnresolvedOperand tile, tiled_tensor;
  Type tile_type, original_type;
  SmallVector<OpAsmParser::UnresolvedOperand, 4> offsets;
  if (parser.parseOperand(tile) || parser.parseKeyword("into") ||
      parser.parseOperand(tiled_tensor) ||
      parser.parseOperandList(offsets, OpAsmParser::Delimiter::Square) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(tile_type) || parser.parseKeyword("into") ||
      parser.parseType(original_type) ||
      parser.resolveOperand(tile, tile_type, result.operands)) {
    return failure();
  }
  auto tiled_tensor_type = TiledTensorType::get(
      parser.getContext(), mlir::cast<RankedTensorType>(tile_type),
      mlir::cast<RankedTensorType>(original_type));

  auto offset_type = builder.getI32Type();
  if (parser.resolveOperand(tiled_tensor, tiled_tensor_type, result.operands) ||
      parser.resolveOperands(offsets, offset_type, result.operands)) {
    return failure();
  }
  result.addTypes(original_type);
  return success();
}

void InsertOp::print(OpAsmPrinter& p) {
  TiledTensorType tiled_type = getDst().getType();
  p << ' ' << getSrc() << " into " << getDst() << "[";
  llvm::interleaveComma(getOffsets(), p);
  p << ']';
  p.printOptionalAttrDict((*this)->getAttrs());
  p << " : " << tiled_type.getTileType() << " into "
    << tiled_type.getOriginalType();
}

LogicalResult InsertOp::verify() {
  if (getSrc().getType().getRank() == 0) {
    return emitError("cannot insert a 0-d tensor");
  }
  if (getDst().getType().getRank() != getOffsets().size())
    return emitError(
        "destination tensor rank does not match number of offsets");
  return success();
}

}  // namespace mlir::triton::xla

#define GET_OP_CLASSES
#include "xla/backends/gpu/codegen/triton/ir/triton_xla_ops.cc.inc"
