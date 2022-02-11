/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/functions/lib/LambdaFunctionUtil.h"

namespace facebook::velox::functions {
namespace {
BufferPtr flattenNulls(
    const SelectivityVector& rows,
    const DecodedVector& decodedVector) {
  if (!decodedVector.mayHaveNulls()) {
    return BufferPtr(nullptr);
  }

  BufferPtr nulls =
      AlignedBuffer::allocate<bool>(rows.size(), decodedVector.base()->pool());
  auto rawNulls = nulls->asMutable<uint64_t>();
  rows.applyToSelected([&](vector_size_t row) {
    bits::setNull(rawNulls, row, decodedVector.isNullAt(row));
  });
  return nulls;
}

template <typename T>
void flattenBuffers(
    const SelectivityVector& rows,
    vector_size_t newNumElements,
    DecodedVector& decodedVector,
    BufferPtr& newNulls,
    BufferPtr& elementIndices,
    BufferPtr& newSizes,
    BufferPtr& newOffsets) {
  auto pool = decodedVector.base()->pool();

  newNulls = flattenNulls(rows, decodedVector);
  uint64_t* rawNewNulls = newNulls ? newNulls->asMutable<uint64_t>() : nullptr;

  elementIndices = allocateIndices(newNumElements, pool);
  auto rawElementIndices = elementIndices->asMutable<vector_size_t>();
  newSizes = allocateSizes(rows.end(), pool);
  auto rawNewSizes = newSizes->asMutable<vector_size_t>();
  newOffsets = allocateOffsets(rows.end(), pool);
  auto rawNewOffsets = newOffsets->asMutable<vector_size_t>();

  auto indices = decodedVector.indices();
  auto vector = decodedVector.base()->as<T>();
  auto rawSizes = vector->rawSizes();
  auto rawOffsets = vector->rawOffsets();

  vector_size_t elementIndex = 0;
  rows.applyToSelected([&](vector_size_t row) {
    if (rawNewNulls && bits::isBitNull(rawNewNulls, row)) {
      return;
    }
    auto size = rawSizes[indices[row]];
    auto offset = rawOffsets[indices[row]];
    rawNewSizes[row] = size;
    rawNewOffsets[row] = elementIndex;

    for (auto i = 0; i < size; i++) {
      rawElementIndices[elementIndex++] = offset + i;
    }
  });
}
} // namespace

ArrayVectorPtr flattenArray(
    const SelectivityVector& rows,
    const VectorPtr& vector,
    DecodedVector& decodedVector) {
  if (decodedVector.isIdentityMapping()) {
    return std::dynamic_pointer_cast<ArrayVector>(vector);
  }

  auto newNumElements = countElements<ArrayVector>(rows, decodedVector);

  BufferPtr newNulls;
  BufferPtr elementIndices;
  BufferPtr newSizes;
  BufferPtr newOffsets;
  flattenBuffers<ArrayVector>(
      rows,
      newNumElements,
      decodedVector,
      newNulls,
      elementIndices,
      newSizes,
      newOffsets);

  auto array = decodedVector.base()->as<ArrayVector>();
  return std::make_shared<ArrayVector>(
      array->pool(),
      array->type(),
      newNulls,
      rows.size(),
      newOffsets,
      newSizes,
      BaseVector::wrapInDictionary(
          BufferPtr(nullptr),
          elementIndices,
          newNumElements,
          array->elements()));
}

MapVectorPtr flattenMap(
    const SelectivityVector& rows,
    const VectorPtr& vector,
    DecodedVector& decodedVector) {
  if (decodedVector.isIdentityMapping()) {
    return std::dynamic_pointer_cast<MapVector>(vector);
  }

  auto newNumElements = countElements<MapVector>(rows, decodedVector);

  BufferPtr newNulls;
  BufferPtr elementIndices;
  BufferPtr newSizes;
  BufferPtr newOffsets;
  flattenBuffers<MapVector>(
      rows,
      newNumElements,
      decodedVector,
      newNulls,
      elementIndices,
      newSizes,
      newOffsets);

  auto map = decodedVector.base()->as<MapVector>();
  return std::make_shared<MapVector>(
      map->pool(),
      map->type(),
      newNulls,
      rows.size(),
      newOffsets,
      newSizes,
      BaseVector::wrapInDictionary(
          BufferPtr(nullptr), elementIndices, newNumElements, map->mapKeys()),
      BaseVector::wrapInDictionary(
          BufferPtr(nullptr),
          elementIndices,
          newNumElements,
          map->mapValues()));
}

template <typename T, typename TVector>
void generateSet(
    const ArrayVector* arrayVector,
    const TVector* arrayElements,
    vector_size_t idx,
    SetWithNull<T>& rightSet) {
  auto size = arrayVector->sizeAt(idx);
  auto offset = arrayVector->offsetAt(idx);
  rightSet.reset();

  for (vector_size_t i = offset; i < (offset + size); ++i) {
    if (arrayElements->isNullAt(i)) {
      rightSet.hasNull = true;
    } else {
      // Function can be called with either FlatVector or DecodedVector, but
      // their APIs are slightly different.
      if constexpr (std::is_same_v<TVector, DecodedVector>) {
        rightSet.set.insert(arrayElements->template valueAt<T>(i));
      } else {
        rightSet.set.insert(arrayElements->valueAt(i));
      }
    }
  }
}

DecodedVector* getDecodedElementsFromArrayVector(
    exec::EvalCtx* context,
    const BaseVector& vector,
    const SelectivityVector& rows) {
  exec::LocalDecodedVector decoder(context, vector, rows);
  auto decodedVector = decoder.get();
  auto baseArrayVector = decoder->base()->as<ArrayVector>();

  // Decode and acquire array elements vector.
  auto elementsVector = baseArrayVector->elements();
  auto elementsRows = toElementRows(
      elementsVector->size(), rows, baseArrayVector, decodedVector->indices());
  exec::LocalDecodedVector decodedElements(
      context, *elementsVector, elementsRows);
  auto decodedElementsVector = decodedElements.get();
  return decodedElementsVector;
}

void validateType(
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const std::string name,
    const size_t expectedArgCount) {
  VELOX_USER_CHECK_EQ(
      inputArgs.size(),
      expectedArgCount,
      "{} requires exactly two parameters",
      name);

  auto arrayType = inputArgs.front().type;
  VELOX_USER_CHECK_EQ(
      arrayType->kind(),
      TypeKind::ARRAY,
      "{} requires arguments of type ARRAY",
      name);

  for (auto& arg : inputArgs) {
    VELOX_USER_CHECK(
        arrayType->kindEquals(arg.type),
        "{} function requires all arguments of the same type: {} vs. {}",
        name,
        arg.type->toString(),
        arrayType->toString());
  }
}
} // namespace facebook::velox::functions
