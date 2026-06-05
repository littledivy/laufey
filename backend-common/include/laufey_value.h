// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Canonical laufey value type plus the shared value_* marshalling implementation.
// Both the CEF and webview backends store values as laufey::Value and install the
// same value_* function pointers via laufey_register_value_api(). A backend that
// needs a different native representation at its IPC boundary (e.g. CEF's
// CefValue) converts to/from laufey::Value only at that boundary, not per call.

#ifndef LAUFEY_BACKEND_COMMON_LAUFEY_VALUE_H_
#define LAUFEY_BACKEND_COMMON_LAUFEY_VALUE_H_

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "laufey.h"

namespace laufey {

struct Value;
using ValuePtr = std::shared_ptr<Value>;
using ValueList = std::vector<ValuePtr>;
using ValueDict = std::map<std::string, ValuePtr>;

enum class ValueType {
  Null,
  Bool,
  Int,
  Double,
  String,
  Binary,
  List,
  Dict,
  Callback
};

struct BinaryData {
  std::vector<uint8_t> data;
};

struct Value {
  ValueType type;
  std::variant<std::nullptr_t, bool, int, double, std::string, BinaryData,
               ValueList, ValueDict, uint64_t>
      data;

  Value() : type(ValueType::Null), data(nullptr) {}

  static ValuePtr Null() {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Null;
    v->data = nullptr;
    return v;
  }

  static ValuePtr Bool(bool val) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Bool;
    v->data = val;
    return v;
  }

  static ValuePtr Int(int val) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Int;
    v->data = val;
    return v;
  }

  static ValuePtr Double(double val) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Double;
    v->data = val;
    return v;
  }

  static ValuePtr String(const std::string& val) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::String;
    v->data = val;
    return v;
  }

  static ValuePtr Binary(const void* data, size_t len) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Binary;
    BinaryData bd;
    bd.data.resize(len);
    if (data && len > 0) {
      memcpy(bd.data.data(), data, len);
    }
    v->data = std::move(bd);
    return v;
  }

  static ValuePtr List() {
    auto v = std::make_shared<Value>();
    v->type = ValueType::List;
    v->data = ValueList{};
    return v;
  }

  static ValuePtr Dict() {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Dict;
    v->data = ValueDict{};
    return v;
  }

  static ValuePtr Callback(uint64_t callback_id) {
    auto v = std::make_shared<Value>();
    v->type = ValueType::Callback;
    v->data = callback_id;
    return v;
  }

  bool IsNull() const {
    return type == ValueType::Null;
  }
  bool IsBool() const {
    return type == ValueType::Bool;
  }
  bool IsInt() const {
    return type == ValueType::Int;
  }
  bool IsDouble() const {
    return type == ValueType::Double;
  }
  bool IsString() const {
    return type == ValueType::String;
  }
  bool IsBinary() const {
    return type == ValueType::Binary;
  }
  bool IsList() const {
    return type == ValueType::List;
  }
  bool IsDict() const {
    return type == ValueType::Dict;
  }
  bool IsCallback() const {
    return type == ValueType::Callback;
  }

  bool GetBool() const {
    return type == ValueType::Bool ? std::get<bool>(data) : false;
  }

  int GetInt() const {
    return type == ValueType::Int ? std::get<int>(data) : 0;
  }

  double GetDouble() const {
    return type == ValueType::Double ? std::get<double>(data) : 0.0;
  }

  const std::string& GetString() const {
    static const std::string empty;
    return type == ValueType::String ? std::get<std::string>(data) : empty;
  }

  const BinaryData& GetBinary() const {
    static const BinaryData empty;
    return type == ValueType::Binary ? std::get<BinaryData>(data) : empty;
  }

  ValueList& GetList() {
    return std::get<ValueList>(data);
  }

  const ValueList& GetList() const {
    static const ValueList empty;
    return type == ValueType::List ? std::get<ValueList>(data) : empty;
  }

  ValueDict& GetDict() {
    return std::get<ValueDict>(data);
  }

  const ValueDict& GetDict() const {
    static const ValueDict empty;
    return type == ValueType::Dict ? std::get<ValueDict>(data) : empty;
  }

  uint64_t GetCallbackId() const {
    return type == ValueType::Callback ? std::get<uint64_t>(data) : 0;
  }
};

}  // namespace laufey

struct laufey_value {
  laufey::ValuePtr value;
  bool is_callback;
  uint64_t callback_id;

  laufey_value() : is_callback(false), callback_id(0) {}
  explicit laufey_value(laufey::ValuePtr v)
      : value(v), is_callback(false), callback_id(0) {
    if (v && v->IsCallback()) {
      is_callback = true;
      callback_id = v->GetCallbackId();
    }
  }

  static laufey_value* CreateCallback(uint64_t id) {
    auto val = laufey::Value::Callback(id);
    laufey_value* v = new laufey_value(val);
    v->is_callback = true;
    v->callback_id = id;
    return v;
  }
};

// Install the shared value_* marshalling implementation into `api`. Call once
// while a backend initializes its laufey_backend_api_t.
void laufey_register_value_api(laufey_backend_api_t* api);

#endif  // LAUFEY_VALUE_H_
