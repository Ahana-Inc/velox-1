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
#include "velox/duckdb/conversion/DuckWrapper.h"
#include "velox/common/base/BitUtil.h"
#include "velox/duckdb/conversion/DuckConversion.h"
#include "velox/external/duckdb/duckdb.hpp"
#include "velox/external/duckdb/tpch/include/tpch-extension.hpp"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::duckdb {
using ::duckdb::Connection;
using ::duckdb::DataChunk;
using ::duckdb::DuckDB;
using ::duckdb::Hugeint;
using ::duckdb::hugeint_t;
using ::duckdb::LogicalTypeId;
using ::duckdb::PhysicalType;
using ::duckdb::QueryResult;

namespace {

class DuckDBBufferReleaser {
 public:
  explicit DuckDBBufferReleaser(
      ::duckdb::buffer_ptr<::duckdb::VectorBuffer> buffer)
      : buffer_(std::move(buffer)) {}

  void addRef() const {}
  void release() const {}

 private:
  const ::duckdb::buffer_ptr<::duckdb::VectorBuffer> buffer_;
};

class DuckDBValidityReleaser {
 public:
  explicit DuckDBValidityReleaser(const ::duckdb::ValidityMask& validity)
      : validity_(validity) {}

  void addRef() const {}
  void release() const {}

 private:
  const ::duckdb::ValidityMask validity_;
};

} // namespace

DuckDBWrapper::DuckDBWrapper(core::ExecCtx* context, const char* path)
    : context_(context) {
  db_ = std::make_unique<DuckDB>(path);
  connection_ = std::make_unique<Connection>(*db_);
  db_->LoadExtension<::duckdb::TPCHExtension>();
}

DuckDBWrapper::~DuckDBWrapper() {}

std::unique_ptr<DuckResult> DuckDBWrapper::execute(const std::string& query) {
  auto duckResult = connection_->Query(query);
  return std::make_unique<DuckResult>(context_, move(duckResult));
}

void DuckDBWrapper::print(const std::string& query) {
  auto result = connection_->Query(query);
  result->Print();
}

DuckResult::DuckResult(
    core::ExecCtx* context,
    std::unique_ptr<QueryResult> queryResult)
    : context_(context), queryResult_(std::move(queryResult)) {
  auto columnCount = queryResult_->types.size();

  std::vector<std::string> names;
  std::vector<TypePtr> types;
  names.reserve(columnCount);
  types.reserve(columnCount);
  for (auto i = 0; i < columnCount; i++) {
    types.push_back(getType(i));
    names.push_back(getName(i));
  }
  type_ = std::make_shared<RowType>(move(names), move(types));
}

DuckResult::~DuckResult() {}

bool DuckResult::success() {
  return queryResult_->success;
}

std::string DuckResult::errorMessage() {
  return queryResult_->error;
}

RowVectorPtr DuckResult::getVector() {
  auto rowType = getType();
  std::vector<VectorPtr> outputColumns;
  outputColumns.reserve(columnCount());
  for (auto i = 0; i < columnCount(); i++) {
    outputColumns.push_back(getVector(i));
  }

  return std::make_shared<RowVector>(
      context_->pool(),
      rowType,
      BufferPtr(nullptr),
      currentChunk_->size(),
      outputColumns);
}

TypePtr DuckResult::getType(size_t columnIdx) {
  assert(columnIdx < queryResult_->types.size());
  return toVeloxType(queryResult_->types[columnIdx]);
}

std::string DuckResult::getName(size_t columnIdx) {
  assert(columnIdx < queryResult_->names.size());
  return queryResult_->names[columnIdx];
}

template <class OP>
VectorPtr convert(
    ::duckdb::Vector& duckVector,
    const TypePtr& veloxType,
    size_t size,
    memory::MemoryPool* pool,
    uint8_t* validity = nullptr) {
  auto vectorType = duckVector.GetVectorType();
  switch (vectorType) {
    case ::duckdb::VectorType::FLAT_VECTOR: {
      VectorPtr result;
      auto& duckValidity = ::duckdb::FlatVector::Validity(duckVector);
      auto* duckData =
          ::duckdb::FlatVector::GetData<typename OP::DUCK_TYPE>(duckVector);

      // Some DuckDB vectors have different internal layout and cannot be
      // trivially copied.
      if (duckVector.GetType() == LogicalTypeId::HUGEINT ||
          duckVector.GetType() == LogicalTypeId::TIMESTAMP ||
          duckVector.GetType() == LogicalTypeId::VARCHAR) {
        // TODO Figure out how to perform a zero-copy conversion.
        result = BaseVector::create(veloxType, size, pool);
        auto flatResult = result->as<FlatVector<typename OP::VELOX_TYPE>>();

        for (auto i = 0; i < size; i++) {
          if (duckValidity.RowIsValid(i) &&
              (!validity || bits::isBitSet(validity, i))) {
            flatResult->set(i, OP::toVelox(duckData[i]));
          }
        }

        if (!duckValidity.AllValid()) {
          auto rawNulls = flatResult->mutableRawNulls();
          memcpy(rawNulls, duckValidity.GetData(), bits::nbytes(size));
        }
      } else {
        auto valuesView = BufferView<DuckDBBufferReleaser>::create(
            reinterpret_cast<const uint8_t*>(duckData),
            size * sizeof(typename OP::VELOX_TYPE),
            DuckDBBufferReleaser(duckVector.GetBuffer()));

        BufferPtr nullsView(nullptr);
        if (!duckValidity.AllValid()) {
          nullsView = BufferView<DuckDBValidityReleaser>::create(
              reinterpret_cast<const uint8_t*>(duckValidity.GetData()),
              bits::nbytes(size),
              DuckDBValidityReleaser(duckValidity));
        }

        result = std::make_shared<FlatVector<typename OP::VELOX_TYPE>>(
            pool, nullsView, size, valuesView, std::vector<BufferPtr>());
      }

      return result;
    }
    case ::duckdb::VectorType::DICTIONARY_VECTOR: {
      auto& child = ::duckdb::DictionaryVector::Child(duckVector);
      auto& selection = ::duckdb::DictionaryVector::SelVector(duckVector);

      // DuckDB vectors doesn't tell what their size is. We are going to use max
      // index + 1 instead as the vector is guaranteed to be at least that
      // large.
      vector_size_t maxIndex = 0;
      for (auto i = 0; i < size; i++) {
        maxIndex = std::max(maxIndex, (vector_size_t)selection.get_index(i));
      }
      VectorPtr base;
      // Unused dictionary elements can be uninitialized. That can cause
      // errors if we try to decode them. Here we create a bitmap of
      // used values to avoid that.
      if (child.GetType() == LogicalTypeId::HUGEINT ||
          child.GetType() == LogicalTypeId::TIMESTAMP ||
          child.GetType() == LogicalTypeId::VARCHAR) {
        std::vector<uint8_t> validityVector(bits::nbytes(maxIndex + 1), 0);
        auto validity_ptr = validityVector.data();
        for (auto i = 0; i < size; i++) {
          bits::setBit(validity_ptr, selection.get_index(i));
        }
        base = convert<OP>(child, veloxType, maxIndex + 1, pool, validity_ptr);
      } else {
        base = convert<OP>(child, veloxType, maxIndex + 1, pool);
      }

      auto indices = AlignedBuffer::allocate<vector_size_t>(size, pool);
      memcpy(
          indices->asMutable<vector_size_t>(),
          selection.data(),
          size * sizeof(vector_size_t));

      return BaseVector::wrapInDictionary(
          BufferPtr(nullptr), indices, size, base);
    }
    default:
      VELOX_UNSUPPORTED(
          "Unsupported DuckDB vector encoding: {}",
          ::duckdb::VectorTypeToString(vectorType));
  }
}

struct NumericCastToDouble {
  template <class T>
  static double operation(T input) {
    return double(input);
  }
};

template <>
double NumericCastToDouble::operation(hugeint_t input) {
  return Hugeint::Cast<double>(input);
}

template <class OP, typename I>
VectorPtr convertDuckToVeloxDecimal(
    ::duckdb::Vector& duckVector,
    TypePtr veloxType,
    int32_t size,
    memory::MemoryPool* pool,
    uint8_t* validity = nullptr) {
  auto vectorType = duckVector.GetVectorType();
  auto internalType = duckVector.GetType().InternalType();
  switch (vectorType) {
    case ::duckdb::VectorType::FLAT_VECTOR: {
      VectorPtr result;
      auto& duckValidity = ::duckdb::FlatVector::Validity(duckVector);
      auto* duckData = ::duckdb::FlatVector::GetData<I>(duckVector);
      result = BaseVector::create(veloxType, size, pool);
      auto flatResult = result->as<FlatVector<typename OP::VELOX_TYPE>>();
      if (internalType == PhysicalType::INT16 ||
          internalType == PhysicalType::INT32) {
        // Cannot re-use duckdb buffers. Create a copy of the vector.
        for (auto i = 0; i < size; i++) {
          if (duckValidity.RowIsValid(i) &&
              (!validity || bits::isBitSet(validity, i))) {
            flatResult->set(i, OP::toVelox(duckData[i]));
          }
        }
        if (!duckValidity.AllValid()) {
          auto rawNulls = flatResult->mutableRawNulls();
          memcpy(rawNulls, duckValidity.GetData(), bits::nbytes(size));
        }
      } else if (
          internalType == PhysicalType::INT64 ||
          internalType == PhysicalType::INT128) {
        // Re-use duckdb buffers.
        auto valuesView = BufferView<DuckDBBufferReleaser>::create(
            reinterpret_cast<const uint8_t*>(duckData),
            size * sizeof(typename OP::VELOX_TYPE),
            DuckDBBufferReleaser(duckVector.GetBuffer()));
        BufferPtr nullsView(nullptr);
        if (!duckValidity.AllValid()) {
          nullsView = BufferView<DuckDBValidityReleaser>::create(
              reinterpret_cast<const uint8_t*>(duckValidity.GetData()),
              bits::nbytes(size),
              DuckDBValidityReleaser(duckValidity));
        }
        result = std::make_shared<FlatVector<typename OP::VELOX_TYPE>>(
            pool,
            veloxType,
            nullsView,
            size,
            valuesView,
            std::vector<BufferPtr>());
      } else {
        VELOX_UNSUPPORTED(
            "Unsupported DuckDB LogicalType:{} to ShortDecimal "
            "conversion",
            duckVector.GetType().ToString());
      }
      return result;
    }
    case ::duckdb::VectorType::DICTIONARY_VECTOR: {
      auto& child = ::duckdb::DictionaryVector::Child(duckVector);
      auto& selection = ::duckdb::DictionaryVector::SelVector(duckVector);

      // DuckDB vectors doesn't tell what their size is. We are going to use max
      // index + 1 instead as the vector is guaranteed to be at least that
      // large.
      vector_size_t maxIndex = 0;
      for (auto i = 0; i < size; i++) {
        maxIndex = std::max(maxIndex, (vector_size_t)selection.get_index(i));
      }
      // Unused dictionary elements can be uninitialized. That can cause
      // errors if we try to decode them. Here we create a bitmap of
      // used values to avoid that.
      return convertDuckToVeloxDecimal<OP, I>(child, veloxType, size, pool);
    }
    default:
      VELOX_UNSUPPORTED(
          "Unsupported DuckDB vector encoding: {}",
          ::duckdb::VectorTypeToString(vectorType));
  }
}

VectorPtr toVeloxVector(
    int32_t size,
    ::duckdb::Vector& duckVector,
    const TypePtr& veloxType,
    memory::MemoryPool* pool) {
  auto type = duckVector.GetType();
  switch (type.id()) {
    case LogicalTypeId::BOOLEAN:
      return convert<DuckNumericConversion<bool>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::TINYINT:
      return convert<DuckNumericConversion<int8_t>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::SMALLINT:
      return convert<DuckNumericConversion<int16_t>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::INTEGER:
      return convert<DuckNumericConversion<int32_t>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::BIGINT:
      return convert<DuckNumericConversion<int64_t>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::HUGEINT:
      return convert<DuckHugeintConversion>(duckVector, veloxType, size, pool);
    case LogicalTypeId::FLOAT:
      return convert<DuckNumericConversion<float>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::DOUBLE:
      return convert<DuckNumericConversion<double>>(
          duckVector, veloxType, size, pool);
    case LogicalTypeId::DECIMAL: {
      uint8_t width;
      uint8_t scale;
      type.GetDecimalProperties(width, scale);
      auto veloxDecimalType = DECIMAL(width, scale);
      switch (type.InternalType()) {
        case PhysicalType::INT16:
          return convertDuckToVeloxDecimal<DuckShortDecimalConversion, int16_t>(
              duckVector, veloxDecimalType, size, pool);
        case PhysicalType::INT32:
          return convertDuckToVeloxDecimal<DuckShortDecimalConversion, int32_t>(
              duckVector, veloxDecimalType, size, pool);
        case PhysicalType::INT64:
          return convertDuckToVeloxDecimal<DuckShortDecimalConversion, int64_t>(
              duckVector, veloxDecimalType, size, pool);
        case PhysicalType::INT128:
          return convertDuckToVeloxDecimal<
              DuckLongDecimalConversion,
              hugeint_t>(duckVector, veloxDecimalType, size, pool);
        default:
          throw std::runtime_error(
              "unrecognized internal type for decimal (this shouldn't happen");
      }
    }
    case LogicalTypeId::VARCHAR:
      return convert<DuckStringConversion>(duckVector, veloxType, size, pool);
    case LogicalTypeId::DATE:
      return convert<DuckDateConversion>(duckVector, veloxType, size, pool);
    case LogicalTypeId::TIMESTAMP:
      return convert<DuckTimestampConversion>(
          duckVector, veloxType, size, pool);
    default:
      throw std::runtime_error(
          "Unsupported vector type for conversion: " + type.ToString());
  }
}

VectorPtr DuckResult::getVector(size_t columnIdx) {
  VELOX_CHECK_LT(columnIdx, columnCount());
  VELOX_CHECK(
      currentChunk_,
      "no chunk available: did you call next() and did it return true?");
  auto& duckVector = currentChunk_->data[columnIdx];
  auto resultType = getType(columnIdx);
  return toVeloxVector(
      currentChunk_->size(), duckVector, resultType, context_->pool());
}

bool DuckResult::next() {
  currentChunk_ = queryResult_->Fetch();
  if (!currentChunk_) {
    return false;
  }
  currentChunk_->Normalify();
  return currentChunk_->size() > 0;
}

} // namespace facebook::velox::duckdb
