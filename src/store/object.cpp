#include "store/object.h"

#include "util/strings.h"

namespace credis {

std::string_view object_type_name(ObjectType type) {
  switch (type) {
    case ObjectType::String: return "string";
    case ObjectType::List: return "list";
    case ObjectType::Hash: return "hash";
    case ObjectType::Set: return "set";
    case ObjectType::ZSet: return "zset";
  }
  return "unknown";
}

Object Object::clone() const {
  switch (type()) {
    case ObjectType::String:
      return make_string(as_string());
    case ObjectType::List: {
      Object copy = make_list();
      copy.as_list() = as_list();
      return copy;
    }
    case ObjectType::Hash: {
      Object copy = make_hash();
      Hash& target = copy.as_hash();
      as_hash().for_each([&](const std::string& field, const std::string& value) {
        target.insert(field, value);
      });
      return copy;
    }
    case ObjectType::Set: {
      Object copy = make_set();
      Set& target = copy.as_set();
      as_set().for_each([&](const std::string& member, const Empty&) { target.insert(member); });
      return copy;
    }
    case ObjectType::ZSet: {
      Object copy = make_zset();
      ZSet& target = copy.as_zset();
      // Walking the skiplist copies in sorted order, which keeps the clone's
      // level structure similar to the original's.
      for (const auto* node = as_zset().skiplist().first(); node != nullptr;
           node = node->levels[0].forward) {
        target.add(node->member, node->score);
      }
      return copy;
    }
  }
  return Object();
}

size_t Object::element_count() const {
  switch (type()) {
    case ObjectType::String: return as_string().size();
    case ObjectType::List: return as_list().size();
    case ObjectType::Hash: return as_hash().size();
    case ObjectType::Set: return as_set().size();
    case ObjectType::ZSet: return as_zset().size();
  }
  return 0;
}

bool Object::is_empty_container() const {
  switch (type()) {
    case ObjectType::String: return false;  // an empty string is still a value
    case ObjectType::List: return as_list().empty();
    case ObjectType::Hash: return as_hash().empty();
    case ObjectType::Set: return as_set().empty();
    case ObjectType::ZSet: return as_zset().empty();
  }
  return false;
}

std::string_view Object::encoding() const {
  switch (type()) {
    case ObjectType::String:
      // Redis reports "int" for values it stores as a long long, and
      // "embstr"/"raw" by length. credis keeps every string as std::string but
      // reports the same labels so clients see familiar answers.
      if (string2ll(as_string(), nullptr)) return "int";
      return as_string().size() <= 44 ? "embstr" : "raw";
    case ObjectType::List: return "listpack";
    case ObjectType::Hash: return "hashtable";
    case ObjectType::Set: return "hashtable";
    case ObjectType::ZSet: return "skiplist";
  }
  return "unknown";
}

}  // namespace credis
