// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

// Shared implementation of the laufey_backend_api_t value_* marshalling surface.
// Operates entirely on laufey::Value so every backend links one copy instead of
// hand-duplicating these ~35 functions.

#include "laufey_value.h"

#include <cstdlib>

namespace {

bool ValueIsNull(laufey_value_t* val) {
  if (!val || !val->value)
    return true;
  return val->value->IsNull();
}

bool ValueIsBool(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsBool();
}

bool ValueIsInt(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsInt();
}

bool ValueIsDouble(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsDouble();
}

bool ValueIsString(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsString();
}

bool ValueIsList(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsList();
}

bool ValueIsDict(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsDict();
}

bool ValueIsBinary(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->IsBinary();
}

bool ValueIsCallback(laufey_value_t* val) {
  if (!val)
    return false;
  return val->is_callback;
}

bool ValueGetBool(laufey_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetBool();
}

int ValueGetInt(laufey_value_t* val) {
  if (!val || !val->value)
    return 0;
  return val->value->GetInt();
}

double ValueGetDouble(laufey_value_t* val) {
  if (!val || !val->value)
    return 0.0;
  return val->value->GetDouble();
}

char* ValueGetString(laufey_value_t* val, size_t* len_out) {
  if (!val || !val->value || !val->value->IsString()) {
    if (len_out)
      *len_out = 0;
    return nullptr;
  }
  const std::string& str = val->value->GetString();
  if (len_out)
    *len_out = str.size();
  char* result = static_cast<char*>(malloc(str.size() + 1));
  if (result) {
    memcpy(result, str.c_str(), str.size() + 1);
  }
  return result;
}

void ValueFreeString(char* str) {
  free(str);
}

size_t ValueListSize(laufey_value_t* val) {
  if (!val || !val->value || !val->value->IsList())
    return 0;
  return val->value->GetList().size();
}

laufey_value_t* ValueListGet(laufey_value_t* val, size_t index) {
  if (!val || !val->value || !val->value->IsList())
    return nullptr;
  const auto& list = val->value->GetList();
  if (index >= list.size())
    return nullptr;
  return new laufey_value(list[index]);
}

laufey_value_t* ValueDictGet(laufey_value_t* dict, const char* key) {
  if (!dict || !dict->value || !dict->value->IsDict() || !key)
    return nullptr;
  const auto& d = dict->value->GetDict();
  auto it = d.find(key);
  if (it == d.end())
    return nullptr;
  return new laufey_value(it->second);
}

bool ValueDictHas(laufey_value_t* dict, const char* key) {
  if (!dict || !dict->value || !dict->value->IsDict() || !key)
    return false;
  const auto& d = dict->value->GetDict();
  return d.find(key) != d.end();
}

size_t ValueDictSize(laufey_value_t* dict) {
  if (!dict || !dict->value || !dict->value->IsDict())
    return 0;
  return dict->value->GetDict().size();
}

char** ValueDictKeys(laufey_value_t* dict, size_t* count_out) {
  if (!dict || !dict->value || !dict->value->IsDict()) {
    if (count_out)
      *count_out = 0;
    return nullptr;
  }
  const auto& d = dict->value->GetDict();
  if (d.empty()) {
    if (count_out)
      *count_out = 0;
    return nullptr;
  }

  char** result = static_cast<char**>(malloc(sizeof(char*) * d.size()));
  if (!result) {
    if (count_out)
      *count_out = 0;
    return nullptr;
  }
  size_t i = 0;
  for (const auto& pair : d) {
    result[i] = static_cast<char*>(malloc(pair.first.size() + 1));
    if (!result[i]) {
      // Roll back rather than hand back a half-built array.
      for (size_t j = 0; j < i; ++j)
        free(result[j]);
      free(result);
      if (count_out)
        *count_out = 0;
      return nullptr;
    }
    memcpy(result[i], pair.first.c_str(), pair.first.size() + 1);
    ++i;
  }
  if (count_out)
    *count_out = d.size();
  return result;
}

void ValueFreeKeys(char** keys, size_t count) {
  if (!keys)
    return;
  for (size_t i = 0; i < count; ++i) {
    free(keys[i]);
  }
  free(keys);
}

const void* ValueGetBinary(laufey_value_t* val, size_t* len_out) {
  if (!val || !val->value || !val->value->IsBinary()) {
    if (len_out)
      *len_out = 0;
    return nullptr;
  }
  const auto& binary = val->value->GetBinary();
  if (len_out)
    *len_out = binary.data.size();
  return binary.data.data();
}

uint64_t ValueGetCallbackId(laufey_value_t* val) {
  if (!val || !val->is_callback)
    return 0;
  return val->callback_id;
}

laufey_value_t* ValueNull(void*) {
  return new laufey_value(laufey::Value::Null());
}

laufey_value_t* ValueBool(void*, bool v) {
  return new laufey_value(laufey::Value::Bool(v));
}

laufey_value_t* ValueInt(void*, int v) {
  return new laufey_value(laufey::Value::Int(v));
}

laufey_value_t* ValueDouble(void*, double v) {
  return new laufey_value(laufey::Value::Double(v));
}

laufey_value_t* ValueString(void*, const char* v) {
  return new laufey_value(laufey::Value::String(v ? v : ""));
}

laufey_value_t* ValueList(void*) {
  return new laufey_value(laufey::Value::List());
}

laufey_value_t* ValueDict(void*) {
  return new laufey_value(laufey::Value::Dict());
}

laufey_value_t* ValueBinary(void*, const void* data, size_t len) {
  return new laufey_value(laufey::Value::Binary(data, len));
}

bool ValueListAppend(laufey_value_t* list, laufey_value_t* val) {
  if (!list || !list->value || !list->value->IsList())
    return false;
  if (!val || !val->value)
    return false;
  list->value->GetList().push_back(val->value);
  return true;
}

bool ValueListSet(laufey_value_t* list, size_t index, laufey_value_t* val) {
  if (!list || !list->value || !list->value->IsList())
    return false;
  if (!val || !val->value)
    return false;
  auto& l = list->value->GetList();
  if (index >= l.size()) {
    l.resize(index + 1);
  }
  l[index] = val->value;
  return true;
}

bool ValueDictSet(laufey_value_t* dict, const char* key, laufey_value_t* val) {
  if (!dict || !dict->value || !dict->value->IsDict())
    return false;
  if (!key || !val || !val->value)
    return false;
  dict->value->GetDict()[key] = val->value;
  return true;
}

void ValueFree(laufey_value_t* val) {
  delete val;
}

}  // namespace

void laufey_register_value_api(laufey_backend_api_t* api) {
  api->value_is_null = ValueIsNull;
  api->value_is_bool = ValueIsBool;
  api->value_is_int = ValueIsInt;
  api->value_is_double = ValueIsDouble;
  api->value_is_string = ValueIsString;
  api->value_is_list = ValueIsList;
  api->value_is_dict = ValueIsDict;
  api->value_is_binary = ValueIsBinary;
  api->value_is_callback = ValueIsCallback;

  api->value_get_bool = ValueGetBool;
  api->value_get_int = ValueGetInt;
  api->value_get_double = ValueGetDouble;
  api->value_get_string = ValueGetString;
  api->value_free_string = ValueFreeString;
  api->value_list_size = ValueListSize;
  api->value_list_get = ValueListGet;
  api->value_dict_get = ValueDictGet;
  api->value_dict_has = ValueDictHas;
  api->value_dict_size = ValueDictSize;
  api->value_dict_keys = ValueDictKeys;
  api->value_free_keys = ValueFreeKeys;
  api->value_get_binary = ValueGetBinary;
  api->value_get_callback_id = ValueGetCallbackId;

  api->value_null = ValueNull;
  api->value_bool = ValueBool;
  api->value_int = ValueInt;
  api->value_double = ValueDouble;
  api->value_string = ValueString;
  api->value_list = ValueList;
  api->value_dict = ValueDict;
  api->value_binary = ValueBinary;

  api->value_list_append = ValueListAppend;
  api->value_list_set = ValueListSet;
  api->value_dict_set = ValueDictSet;
  api->value_free = ValueFree;
}
