// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "scheme_handler.h"

#include <algorithm>
#include <cstring>

#include "include/cef_request.h"
#include "include/cef_response.h"
#include "runtime_loader.h"

namespace {

// Flatten a CEF header map into laufey's NUL-separated name\0value\0... wire
// format (lowercasing names to match fetch/HTTP conventions).
std::string FlattenHeaders(const CefRequest::HeaderMap& headers) {
  std::string out;
  for (const auto& [key, value] : headers) {
    std::string name = key.ToString();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    out.append(name);
    out.push_back('\0');
    out.append(value.ToString());
    out.push_back('\0');
  }
  return out;
}

}  // namespace

bool LaufeySchemeHandler::Open(CefRefPtr<CefRequest> request,
                               bool& handle_request,
                               CefRefPtr<CefCallback> callback) {
  method_ = request->GetMethod().ToString();
  url_ = request->GetURL().ToString();

  CefRequest::HeaderMap header_map;
  request->GetHeaderMap(header_map);
  std::string flat_headers = FlattenHeaders(header_map);

  // Buffer the request body up front. CEF makes the full POST data available
  // here, so the runtime's ReadRequestBody pulls become non-blocking copies.
  CefRefPtr<CefPostData> post_data = request->GetPostData();
  if (post_data) {
    CefPostData::ElementVector elements;
    post_data->GetElements(elements);
    for (const auto& element : elements) {
      size_t count = element->GetBytesCount();
      if (count == 0)
        continue;
      size_t offset = request_body_.size();
      request_body_.resize(offset + count);
      element->GetBytes(count, request_body_.data() + offset);
    }
  }

  // Defer until the runtime calls Begin(); resume via open_callback_.
  open_callback_ = callback;
  handle_request = false;

  // Hand ownership of an extra reference to the runtime. Released in
  // FinishResponse(). Keeps the handler alive while the runtime streams.
  this->AddRef();
  RuntimeLoader::GetInstance()->DispatchSchemeRequest(window_id_, this, method_,
                                                      url_, flat_headers);
  return true;
}

void LaufeySchemeHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                             int64_t& response_length,
                                             CefString& redirectUrl) {
  std::lock_guard<std::mutex> lock(mutex_);
  response->SetStatus(status_);

  CefResponse::HeaderMap header_map;
  std::string mime_type;
  for (const auto& [name, value] : response_headers_) {
    if (name == "content-type") {
      // CefResponse exposes mime type separately; the full content-type
      // (incl. charset) still round-trips via SetMimeType.
      mime_type = value;
    }
    header_map.insert({name, value});
  }
  response->SetHeaderMap(header_map);
  if (!mime_type.empty()) {
    response->SetMimeType(mime_type);
  }

  // Streaming: length unknown until FinishResponse.
  response_length = -1;
}

bool LaufeySchemeHandler::Read(void* data_out, int bytes_to_read,
                               int& bytes_read,
                               CefRefPtr<CefResourceReadCallback> callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!response_body_.empty()) {
    size_t n =
        std::min(static_cast<size_t>(bytes_to_read), response_body_.size());
    std::copy(response_body_.begin(), response_body_.begin() + n,
              static_cast<uint8_t*>(data_out));
    response_body_.erase(response_body_.begin(), response_body_.begin() + n);
    bytes_read = static_cast<int>(n);
    return true;
  }

  if (finished_ || cancelled_) {
    bytes_read = 0;
    return false;  // EOF
  }

  // No data yet: keep the output buffer + callback, fill them when
  // WriteResponse/FinishResponse arrives and resume via Continue(n).
  pending_data_ = data_out;
  pending_cap_ = bytes_to_read;
  read_callback_ = callback;
  bytes_read = 0;
  return true;
}

void LaufeySchemeHandler::Cancel() {
  CefRefPtr<CefResourceReadCallback> to_continue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    to_continue = read_callback_;
    read_callback_ = nullptr;
    pending_data_ = nullptr;
  }
  // Wake any parked read so it reports EOF.
  if (to_continue)
    to_continue->Continue(0);
}

intptr_t LaufeySchemeHandler::ReadRequestBody(uint8_t* buf, size_t cap) {
  if (cap == 0)
    return 0;
  size_t remaining = request_body_.size() - request_body_cursor_;
  if (remaining == 0)
    return 0;  // EOF
  size_t n = std::min(cap, remaining);
  std::memcpy(buf, request_body_.data() + request_body_cursor_, n);
  request_body_cursor_ += n;
  return static_cast<intptr_t>(n);
}

void LaufeySchemeHandler::Begin(int status, const char* headers,
                                size_t headers_len) {
  CefRefPtr<CefCallback> to_continue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
    response_headers_.clear();
    // Parse the flat name\0value\0... encoding.
    size_t i = 0;
    while (i < headers_len) {
      size_t name_end = i;
      while (name_end < headers_len && headers[name_end] != '\0')
        name_end++;
      if (name_end >= headers_len)
        break;
      size_t value_start = name_end + 1;
      size_t value_end = value_start;
      while (value_end < headers_len && headers[value_end] != '\0')
        value_end++;
      if (value_end > headers_len)
        break;
      response_headers_.emplace_back(
          std::string(headers + i, name_end - i),
          std::string(headers + value_start, value_end - value_start));
      i = value_end + 1;
    }
    began_ = true;
    to_continue = open_callback_;
    open_callback_ = nullptr;
  }
  if (to_continue)
    to_continue->Continue();
}

intptr_t LaufeySchemeHandler::WriteResponse(const uint8_t* buf, size_t len) {
  CefRefPtr<CefResourceReadCallback> to_continue;
  int to_report = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_)
      return -1;
    response_body_.insert(response_body_.end(), buf, buf + len);
    // Satisfy a parked Read by copying into its output buffer.
    if (read_callback_ && pending_data_ && !response_body_.empty()) {
      size_t n =
          std::min(static_cast<size_t>(pending_cap_), response_body_.size());
      std::copy(response_body_.begin(), response_body_.begin() + n,
                static_cast<uint8_t*>(pending_data_));
      response_body_.erase(response_body_.begin(), response_body_.begin() + n);
      to_report = static_cast<int>(n);
      to_continue = read_callback_;
      read_callback_ = nullptr;
      pending_data_ = nullptr;
    }
  }
  if (to_continue)
    to_continue->Continue(to_report);
  return static_cast<intptr_t>(len);
}

void LaufeySchemeHandler::FinishResponse() {
  CefRefPtr<CefResourceReadCallback> to_continue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    to_continue = read_callback_;
    read_callback_ = nullptr;
    pending_data_ = nullptr;
  }
  // A parked read with no remaining body: report EOF.
  if (to_continue)
    to_continue->Continue(0);
  // Release the reference taken in Open(); may destroy this handler.
  this->Release();
}

CefRefPtr<CefResourceHandler> LaufeySchemeHandlerFactory::Create(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    const CefString& scheme_name, CefRefPtr<CefRequest> request) {
  uint32_t window_id =
      RuntimeLoader::GetInstance()->GetLaufeyIdForBrowser(browser);
  return new LaufeySchemeHandler(window_id);
}
