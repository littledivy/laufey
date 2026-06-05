// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "render_process_handler.h"

CefRefPtr<LaufeyRenderProcessHandler> g_render_handler;

LaufeyPathObject::LaufeyPathObject(std::vector<std::string> path,
                             CefRefPtr<CefFrame> frame)
    : path_(std::move(path)), frame_(frame) {}

bool LaufeyPathObject::Get(const CefString& name,
                        const CefRefPtr<CefV8Value> object,
                        CefRefPtr<CefV8Value>& retval, CefString& exception) {
  std::string prop = name.ToString();

  if (prop == "then" || prop == "catch" || prop == "finally" ||
      prop == "constructor" || prop.empty()) {
    return false;
  }

  std::vector<std::string> new_path = path_;
  new_path.push_back(prop);

  CefRefPtr<LaufeyPathObject> handler = new LaufeyPathObject(new_path, frame_);

  retval = CefV8Value::CreateFunction(prop, handler);

  return true;
}

bool LaufeyPathObject::Set(const CefString& name,
                        const CefRefPtr<CefV8Value> object,
                        const CefRefPtr<CefV8Value> value,
                        CefString& exception) {
  exception = "Cannot set properties on Laufey object";
  return true;
}

bool LaufeyPathObject::Get(int index, const CefRefPtr<CefV8Value> object,
                        CefRefPtr<CefV8Value>& retval, CefString& exception) {
  return false;
}

bool LaufeyPathObject::Set(int index, const CefRefPtr<CefV8Value> object,
                        const CefRefPtr<CefV8Value> value,
                        CefString& exception) {
  exception = "Cannot set index properties on Laufey object";
  return true;
}

bool LaufeyPathObject::Execute(const CefString& name, CefRefPtr<CefV8Value> object,
                            const CefV8ValueList& arguments,
                            CefRefPtr<CefV8Value>& retval,
                            CefString& exception) {
  if (!g_render_handler) {
    exception = "Render handler not initialized";
    return true;
  }

  CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
  if (!context) {
    exception = "No V8 context";
    return true;
  }

  uint64_t call_id = g_render_handler->GetNextCallId();

  retval = CefV8Value::CreatePromise();

  if (!retval || !retval->IsPromise()) {
    exception = "Failed to create Promise";
    return true;
  }

  CefRefPtr<PromiseResolver> resolver =
      new PromiseResolver(call_id, retval, context);

  g_render_handler->StorePendingCall(call_id, resolver);

  CefRefPtr<CefListValue> argsList = CefListValue::Create();
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i]->IsFunction()) {
      uint64_t callback_id =
          g_render_handler->StoreCallback(arguments[i], context);

      CefRefPtr<CefDictionaryValue> callbackRef = CefDictionaryValue::Create();
      callbackRef->SetString("__callback__", std::to_string(callback_id));

      CefRefPtr<CefValue> refValue = CefValue::Create();
      refValue->SetDictionary(callbackRef);
      argsList->SetValue(i, refValue);
    } else {
      argsList->SetValue(i, g_render_handler->V8ValueToCefValue(arguments[i]));
    }
  }

  std::string method_path;
  for (size_t i = 0; i < path_.size(); ++i) {
    if (i > 0)
      method_path += ".";
    method_path += path_[i];
  }

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("laufey_call");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  // IDs are 64-bit; carry as double (exact to 2^53) since CefValue has no
  // int64.
  msgArgs->SetDouble(0, static_cast<double>(call_id));
  msgArgs->SetString(1, method_path);
  msgArgs->SetList(2, argsList);

  frame_->SendProcessMessage(PID_BROWSER, msg);

  return true;
}

PromiseResolver::PromiseResolver(uint64_t call_id,
                                 CefRefPtr<CefV8Value> promise,
                                 CefRefPtr<CefV8Context> context)
    : promise_(promise), context_(context) {}

void PromiseResolver::Resolve(CefRefPtr<CefV8Value> value) {
  if (!promise_ || !context_)
    return;

  context_->Enter();
  promise_->ResolvePromise(value);
  context_->Exit();
}

void PromiseResolver::Reject(const std::string& error) {
  if (!promise_ || !context_)
    return;

  context_->Enter();
  promise_->RejectPromise(error);
  context_->Exit();
}

LaufeyRenderProcessHandler::LaufeyRenderProcessHandler() {
  g_render_handler = this;
}

void LaufeyRenderProcessHandler::OnBrowserCreated(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> extra_info) {
  if (extra_info && extra_info->HasKey("laufey_js_namespace")) {
    browser_namespaces_[browser->GetIdentifier()] =
        extra_info->GetString("laufey_js_namespace").ToString();
  }
}

void LaufeyRenderProcessHandler::OnContextCreated(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefV8Context> context) {
  CefRefPtr<CefV8Value> global = context->GetGlobal();

  std::string ns = "Laufey";
  auto it = browser_namespaces_.find(browser->GetIdentifier());
  if (it != browser_namespaces_.end()) {
    ns = it->second;
  }

  CefRefPtr<LaufeyPathObject> handler = new LaufeyPathObject({}, frame);
  CefRefPtr<CefV8Value> laufey = CefV8Value::CreateObject(nullptr, handler);

  global->SetValue(ns, laufey, V8_PROPERTY_ATTRIBUTE_READONLY);
}

void LaufeyRenderProcessHandler::OnContextReleased(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefV8Context> context) {}

bool LaufeyRenderProcessHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefProcessId source_process, CefRefPtr<CefProcessMessage> message) {
  const std::string& name = message->GetName().ToString();

  if (name == "laufey_response") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t call_id = static_cast<uint64_t>(args->GetDouble(0));
    CefRefPtr<CefValue> result = args->GetValue(1);
    std::string error = args->GetString(2).ToString();

    auto it = pending_calls_.find(call_id);
    if (it != pending_calls_.end()) {
      CefRefPtr<PromiseResolver> resolver = it->second;
      pending_calls_.erase(it);

      CefRefPtr<CefV8Context> context = frame->GetV8Context();
      if (context && context->Enter()) {
        if (error.empty()) {
          CefRefPtr<CefV8Value> v8Result = CefValueToV8Value(result, context);
          resolver->Resolve(v8Result);
        } else {
          resolver->Reject(error);
        }
        context->Exit();
      }
    }
    return true;
  }

  if (name == "laufey_callback") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t callback_id = static_cast<uint64_t>(args->GetDouble(0));
    CefRefPtr<CefListValue> callbackArgs = args->GetList(1);

    auto it = stored_callbacks_.find(callback_id);
    if (it != stored_callbacks_.end()) {
      StoredCallback& cb = it->second;

      if (cb.context && cb.context->Enter()) {
        CefV8ValueList v8Args;
        for (size_t i = 0; i < callbackArgs->GetSize(); ++i) {
          v8Args.push_back(
              CefValueToV8Value(callbackArgs->GetValue(i), cb.context));
        }

        cb.func->ExecuteFunction(nullptr, v8Args);
        cb.context->Exit();
      }
    }
    return true;
  }

  if (name == "laufey_eval") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t eval_id = static_cast<uint64_t>(args->GetDouble(0));
    std::string script = args->GetString(1).ToString();

    CefRefPtr<CefV8Context> context = frame->GetV8Context();
    CefRefPtr<CefProcessMessage> reply =
        CefProcessMessage::Create("laufey_eval_result");
    CefRefPtr<CefListValue> replyArgs = reply->GetArgumentList();
    replyArgs->SetDouble(0, static_cast<double>(eval_id));

    if (context && context->Enter()) {
      CefRefPtr<CefV8Value> retval;
      CefRefPtr<CefV8Exception> exception;
      bool success = context->Eval(script, "", 0, retval, exception);

      if (success && retval) {
        replyArgs->SetValue(1, V8ValueToCefValue(retval));
        replyArgs->SetString(2, "");
      } else if (exception) {
        replyArgs->SetNull(1);
        replyArgs->SetString(2, exception->GetMessage().ToString());
      } else {
        replyArgs->SetNull(1);
        replyArgs->SetString(2, "");
      }
      context->Exit();
    } else {
      replyArgs->SetNull(1);
      replyArgs->SetString(2, "Failed to enter V8 context");
    }

    frame->SendProcessMessage(PID_BROWSER, reply);
    return true;
  }

  if (name == "laufey_release_callback") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t callback_id = static_cast<uint64_t>(args->GetDouble(0));

    stored_callbacks_.erase(callback_id);
    return true;
  }

  return false;
}

void LaufeyRenderProcessHandler::StorePendingCall(
    uint64_t call_id, CefRefPtr<PromiseResolver> resolver) {
  pending_calls_[call_id] = resolver;
}

uint64_t LaufeyRenderProcessHandler::StoreCallback(
    CefRefPtr<CefV8Value> func, CefRefPtr<CefV8Context> context) {
  uint64_t id = next_callback_id_++;
  stored_callbacks_[id] = {func, context};
  return id;
}

uint64_t LaufeyRenderProcessHandler::GetNextCallId() {
  return next_call_id_++;
}

CefRefPtr<CefValue> LaufeyRenderProcessHandler::V8ValueToCefValue(
    CefRefPtr<CefV8Value> v8val) {
  CefRefPtr<CefValue> value = CefValue::Create();

  if (!v8val || v8val->IsUndefined() || v8val->IsNull()) {
    value->SetNull();
  } else if (v8val->IsBool()) {
    value->SetBool(v8val->GetBoolValue());
  } else if (v8val->IsInt()) {
    value->SetInt(v8val->GetIntValue());
  } else if (v8val->IsDouble()) {
    value->SetDouble(v8val->GetDoubleValue());
  } else if (v8val->IsString()) {
    value->SetString(v8val->GetStringValue());
  } else if (v8val->IsArray()) {
    CefRefPtr<CefListValue> list = CefListValue::Create();
    int len = v8val->GetArrayLength();
    for (int i = 0; i < len; ++i) {
      list->SetValue(i, V8ValueToCefValue(v8val->GetValue(i)));
    }
    value->SetList(list);
  } else if (v8val->IsObject()) {
    CefRefPtr<CefDictionaryValue> dict = CefDictionaryValue::Create();
    std::vector<CefString> keys;
    v8val->GetKeys(keys);
    for (const auto& key : keys) {
      dict->SetValue(key, V8ValueToCefValue(v8val->GetValue(key)));
    }
    value->SetDictionary(dict);
  } else if (v8val->IsArrayBuffer()) {
    void* data = v8val->GetArrayBufferData();
    size_t len = v8val->GetArrayBufferByteLength();
    if (data && len > 0) {
      value->SetBinary(CefBinaryValue::Create(data, len));
    } else {
      value->SetNull();
    }
  } else {
    value->SetNull();
  }

  return value;
}

CefRefPtr<CefV8Value> LaufeyRenderProcessHandler::CefValueToV8Value(
    CefRefPtr<CefValue> value, CefRefPtr<CefV8Context> context) {
  if (!value) {
    return CefV8Value::CreateNull();
  }

  switch (value->GetType()) {
    case VTYPE_NULL:
      return CefV8Value::CreateNull();
    case VTYPE_BOOL:
      return CefV8Value::CreateBool(value->GetBool());
    case VTYPE_INT:
      return CefV8Value::CreateInt(value->GetInt());
    case VTYPE_DOUBLE:
      return CefV8Value::CreateDouble(value->GetDouble());
    case VTYPE_STRING:
      return CefV8Value::CreateString(value->GetString());
    case VTYPE_BINARY: {
      CefRefPtr<CefBinaryValue> binary = value->GetBinary();
      size_t size = binary->GetSize();
      std::vector<uint8_t> buffer(size);
      binary->GetData(buffer.data(), size, 0);
      CefRefPtr<CefV8Value> arrayBuffer =
          CefV8Value::CreateArrayBuffer(buffer.data(), size, nullptr);
      return arrayBuffer ? arrayBuffer : CefV8Value::CreateNull();
    }
    case VTYPE_DICTIONARY: {
      CefRefPtr<CefDictionaryValue> dict = value->GetDictionary();
      CefRefPtr<CefV8Value> obj = CefV8Value::CreateObject(nullptr, nullptr);
      CefDictionaryValue::KeyList keys;
      dict->GetKeys(keys);
      for (const auto& key : keys) {
        obj->SetValue(key, CefValueToV8Value(dict->GetValue(key), context),
                      V8_PROPERTY_ATTRIBUTE_NONE);
      }
      return obj;
    }
    case VTYPE_LIST: {
      CefRefPtr<CefListValue> list = value->GetList();
      size_t size = list->GetSize();
      CefRefPtr<CefV8Value> arr =
          CefV8Value::CreateArray(static_cast<int>(size));
      for (size_t i = 0; i < size; ++i) {
        arr->SetValue(static_cast<int>(i),
                      CefValueToV8Value(list->GetValue(i), context));
      }
      return arr;
    }
    default:
      return CefV8Value::CreateNull();
  }
}
