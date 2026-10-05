// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_scheme_registry.h"

namespace laufey_common {

SchemeRegistry::SchemeRegistry() {
  ordered_.push_back(kDefaultAppScheme);
  names_.insert(kDefaultAppScheme);
}

// static
SchemeRegistry* SchemeRegistry::GetInstance() {
  static SchemeRegistry* instance = new SchemeRegistry();
  return instance;
}

bool SchemeRegistry::Add(const std::string& scheme) {
  if (!IsValidSchemeName(scheme)) {
    return false;
  }
  std::string normalized = NormalizeSchemeName(scheme);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!names_.insert(normalized).second) {
    return false;
  }
  ordered_.push_back(normalized);
  return true;
}

bool SchemeRegistry::Contains(const std::string& scheme) const {
  std::string normalized = NormalizeSchemeName(scheme);
  std::lock_guard<std::mutex> lock(mutex_);
  return names_.count(normalized) != 0;
}

std::vector<std::string> SchemeRegistry::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ordered_;
}

size_t SchemeRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ordered_.size();
}

}  // namespace laufey_common
