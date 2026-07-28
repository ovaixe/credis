#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <variant>

#include "store/dict.h"
#include "store/skiplist.h"

namespace credis {

// Order must match the alternatives of Object::Value below.
enum class ObjectType : uint8_t {
  String = 0,
  List = 1,
  Hash = 2,
  Set = 3,
  ZSet = 4,
};

// The name TYPE reports.
std::string_view object_type_name(ObjectType type);

// A value stored under a key.
//
// The container choices differ from real Redis, which switches encodings by size
// (listpack for small hashes, intset for all-integer sets, quicklist for lists).
// credis always uses one representation per type; see README.md.
class Object {
 public:
  using List = std::deque<std::string>;   // O(1) push/pop at both ends
  using Hash = Dict<std::string>;         // field -> value
  using Set = Dict<Empty>;                // member set

  using Value = std::variant<std::string, List, Hash, Set, ZSet>;
  // type() casts the variant index straight to ObjectType, so the two must stay
  // in the same order.
  static_assert(std::variant_size_v<Value> == 5);

  Object() : value_(std::string()) {}

  static Object make_string(std::string value) { return Object(Value(std::move(value))); }
  static Object make_list() { return Object(Value(std::in_place_type<List>)); }
  static Object make_hash() { return Object(Value(std::in_place_type<Hash>)); }
  static Object make_set() { return Object(Value(std::in_place_type<Set>)); }
  static Object make_zset() { return Object(Value(std::in_place_type<ZSet>)); }

  Object(Object&&) = default;
  Object& operator=(Object&&) = default;
  // Copying is explicit: the container types are move-only, and an accidental
  // deep copy of a large value would be an expensive silent mistake.
  Object(const Object&) = delete;
  Object& operator=(const Object&) = delete;

  // Deep copy, for COPY.
  Object clone() const;

  ObjectType type() const { return static_cast<ObjectType>(value_.index()); }
  bool is(ObjectType t) const { return type() == t; }
  std::string_view type_name() const { return object_type_name(type()); }

  // Accessors. Callers must check type() first — commands reply WRONGTYPE before
  // ever reaching these, so a bad access is a server bug, not user input.
  std::string& as_string() { return std::get<std::string>(value_); }
  const std::string& as_string() const { return std::get<std::string>(value_); }
  List& as_list() { return std::get<List>(value_); }
  const List& as_list() const { return std::get<List>(value_); }
  Hash& as_hash() { return std::get<Hash>(value_); }
  const Hash& as_hash() const { return std::get<Hash>(value_); }
  Set& as_set() { return std::get<Set>(value_); }
  const Set& as_set() const { return std::get<Set>(value_); }
  ZSet& as_zset() { return std::get<ZSet>(value_); }
  const ZSet& as_zset() const { return std::get<ZSet>(value_); }

  // Number of elements for containers; string length for strings. Used by the
  // commands that delete a key once it becomes empty.
  size_t element_count() const;
  bool is_empty_container() const;

  // What OBJECT ENCODING reports. credis is honest here rather than pretending
  // to have Redis's compact encodings.
  std::string_view encoding() const;

 private:
  explicit Object(Value value) : value_(std::move(value)) {}

  Value value_;
};

}  // namespace credis
