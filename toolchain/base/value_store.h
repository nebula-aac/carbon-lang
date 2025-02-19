// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CARBON_TOOLCHAIN_BASE_VALUE_STORE_H_
#define CARBON_TOOLCHAIN_BASE_VALUE_STORE_H_

#include <type_traits>

#include "common/check.h"
#include "common/hashtable_key_context.h"
#include "common/ostream.h"
#include "common/set.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "toolchain/base/mem_usage.h"
#include "toolchain/base/yaml.h"

namespace Carbon {

namespace Internal {

// Used as a parent class for non-printable types. This is just for
// std::conditional, not as an API.
class ValueStoreNotPrintable {};
}  // namespace Internal

// A simple wrapper for accumulating values, providing IDs to later retrieve the
// value. This does not do deduplication.
//
// IdT::ValueType must represent the type being indexed.
template <typename IdT>
class ValueStore
    : public std::conditional<
          std::is_base_of_v<Printable<typename IdT::ValueType>,
                            typename IdT::ValueType>,
          Yaml::Printable<ValueStore<IdT>>, Internal::ValueStoreNotPrintable> {
 public:
  using ValueType = typename IdT::ValueType;

  // Typically we want to use `ValueType&` and `const ValueType& to avoid
  // copies, but when the value type is a `StringRef`, we assume external
  // storage for the string data and both our value type and ref type will be
  // `StringRef`. This will preclude mutation of the string data.
  using RefType = std::conditional_t<std::same_as<llvm::StringRef, ValueType>,
                                     llvm::StringRef, ValueType&>;
  using ConstRefType =
      std::conditional_t<std::same_as<llvm::StringRef, ValueType>,
                         llvm::StringRef, const ValueType&>;

  // Stores the value and returns an ID to reference it.
  auto Add(ValueType value) -> IdT {
    IdT id(values_.size());
    // This routine is especially hot and the check here relatively expensive
    // for the value provided, so only do this in debug builds to make tracking
    // down issues easier.
    CARBON_DCHECK(id.index >= 0, "Id overflow");
    values_.push_back(std::move(value));
    return id;
  }

  // Adds a default constructed value and returns an ID to reference it.
  auto AddDefaultValue() -> IdT {
    IdT id(values_.size());
    values_.resize(id.index + 1);
    return id;
  }

  // Returns a mutable value for an ID.
  auto Get(IdT id) -> RefType {
    CARBON_DCHECK(id.index >= 0, "{0}", id);
    return values_[id.index];
  }

  // Returns the value for an ID.
  auto Get(IdT id) const -> ConstRefType {
    CARBON_DCHECK(id.index >= 0, "{0}", id);
    return values_[id.index];
  }

  // Reserves space.
  auto Reserve(size_t size) -> void { values_.reserve(size); }

  // These are to support printable structures, and are not guaranteed.
  auto OutputYaml() const -> Yaml::OutputMapping {
    return Yaml::OutputMapping([&](Yaml::OutputMapping::Map map) {
      for (auto [id, value] : enumerate()) {
        map.Add(PrintToString(id), Yaml::OutputScalar(value));
      }
    });
  }

  // Collects memory usage of the values.
  auto CollectMemUsage(MemUsage& mem_usage, llvm::StringRef label) const
      -> void {
    mem_usage.Collect(label.str(), values_);
  }

  auto array_ref() const -> llvm::ArrayRef<ValueType> { return values_; }
  auto size() const -> size_t { return values_.size(); }

  // Makes an iterable range over pairs of the index and a reference to the
  // value for each value in the store.
  //
  // The range is over references to the values in the store, even if used with
  // `auto` to destructure the pair. In this example, the `value` is a
  // `ConstRefType`:
  // ```
  // for (auto [id, value] : store.enumerate()) { ... }
  // ```
  auto enumerate() const -> auto {
    auto index_to_id = [](auto pair) -> std::pair<IdT, ConstRefType> {
      auto [index, value] = pair;
      return std::pair<IdT, ConstRefType>(IdT(index), value);
    };
    auto range = llvm::enumerate(values_);
    using Iter =
        llvm::mapped_iterator<decltype(range.begin()), decltype(index_to_id)>;
    auto begin = Iter(range.begin(), index_to_id);
    auto end = Iter(range.end(), index_to_id);
    return llvm::make_range(begin, end);
  }

 private:
  // Set inline size to 0 because these will typically be too large for the
  // stack, while this does make File smaller.
  llvm::SmallVector<std::decay_t<ValueType>, 0> values_;
};

// A wrapper for accumulating immutable values with deduplication, providing IDs
// to later retrieve the value.
//
// IdT::ValueType must represent the type being indexed.
template <typename IdT>
class CanonicalValueStore {
 public:
  using ValueType = typename IdT::ValueType;
  using RefType = typename ValueStore<IdT>::RefType;
  using ConstRefType = typename ValueStore<IdT>::ConstRefType;

  // Stores a canonical copy of the value and returns an ID to reference it.
  auto Add(ValueType value) -> IdT;

  // Returns the value for an ID.
  auto Get(IdT id) const -> ConstRefType { return values_.Get(id); }

  // Looks up the canonical ID for a value, or returns `None` if not in the
  // store.
  auto Lookup(ValueType value) const -> IdT;

  // Reserves space.
  auto Reserve(size_t size) -> void;

  // These are to support printable structures, and are not guaranteed.
  auto OutputYaml() const -> Yaml::OutputMapping {
    return values_.OutputYaml();
  }

  auto array_ref() const -> llvm::ArrayRef<ValueType> {
    return values_.array_ref();
  }
  auto size() const -> size_t { return values_.size(); }

  // Collects memory usage of the values and deduplication set.
  auto CollectMemUsage(MemUsage& mem_usage, llvm::StringRef label) const
      -> void {
    mem_usage.Collect(MemUsage::ConcatLabel(label, "values_"), values_);
    auto bytes =
        set_.ComputeMetrics(KeyContext(values_.array_ref())).storage_bytes;
    mem_usage.Add(MemUsage::ConcatLabel(label, "set_"), bytes, bytes);
  }

 private:
  class KeyContext;

  ValueStore<IdT> values_;
  Set<IdT, /*SmallSize=*/0, KeyContext> set_;
};

template <typename IdT>
class CanonicalValueStore<IdT>::KeyContext
    : public TranslatingKeyContext<KeyContext> {
 public:
  explicit KeyContext(llvm::ArrayRef<ValueType> values) : values_(values) {}

  // Note that it is safe to return a `const` reference here as the underlying
  // object's lifetime is provided by the `store_`.
  auto TranslateKey(IdT id) const -> const ValueType& {
    return values_[id.index];
  }

 private:
  llvm::ArrayRef<ValueType> values_;
};

template <typename IdT>
auto CanonicalValueStore<IdT>::Add(ValueType value) -> IdT {
  auto make_key = [&] { return IdT(values_.Add(std::move(value))); };
  return set_.Insert(value, make_key, KeyContext(values_.array_ref())).key();
}

template <typename IdT>
auto CanonicalValueStore<IdT>::Lookup(ValueType value) const -> IdT {
  if (auto result = set_.Lookup(value, KeyContext(values_.array_ref()))) {
    return result.key();
  }
  return IdT::None;
}

template <typename IdT>
auto CanonicalValueStore<IdT>::Reserve(size_t size) -> void {
  // Compute the resulting new insert count using the size of values -- the
  // set doesn't have a fast to compute current size.
  if (size > values_.size()) {
    set_.GrowForInsertCount(size - values_.size(),
                            KeyContext(values_.array_ref()));
  }
  values_.Reserve(size);
}

}  // namespace Carbon

#endif  // CARBON_TOOLCHAIN_BASE_VALUE_STORE_H_
