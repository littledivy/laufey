// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Per-app persistent web-data directory. See "App data directory" in
// laufey_backend_common.h and docs/app-data.md.

#include "laufey_backend_common.h"

#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace laufey_common {

namespace {

bool IsSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

std::string StripTrailingSeparators(std::string path) {
  // Keep a bare root ("/", "C:\") intact.
  size_t min_len = 1;
#ifdef _WIN32
  if (path.size() >= 3 && path[1] == ':')
    min_len = 3;
#endif
  while (path.size() > min_len && IsSeparator(path.back()))
    path.pop_back();
  return path;
}

std::string GetEnvUtf8(const char* name) {
#ifdef _WIN32
  std::wstring wname = Utf8ToWide(name);
  DWORD len = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (len == 0)
    return std::string();
  std::wstring value(len, L'\0');
  len = GetEnvironmentVariableW(wname.c_str(), &value[0], len);
  if (len == 0 || len >= value.size())
    return std::string();
  value.resize(len);
  return WideToUtf8(value);
#else
  const char* value = getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

// The per-user base directory app ids are resolved under, or "" if it can't
// be determined.
std::string PlatformAppDataBase() {
#ifdef _WIN32
  PWSTR path = nullptr;
  std::string result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                     nullptr, &path)) &&
      path) {
    result = WideToUtf8(path);
  }
  CoTaskMemFree(path);
  return result;
#else
#ifdef __linux__
  std::string xdg = GetEnvUtf8("XDG_DATA_HOME");
  // The XDG spec says relative values are invalid and must be ignored.
  if (IsAbsolutePath(xdg))
    return StripTrailingSeparators(xdg);
#endif
  std::string home = GetEnvUtf8("HOME");
  if (!IsAbsolutePath(home)) {
    struct passwd* pw = getpwuid(getuid());
    home = (pw && pw->pw_dir) ? pw->pw_dir : "";
  }
  if (!IsAbsolutePath(home))
    return std::string();
  home = StripTrailingSeparators(home);
#ifdef __APPLE__
  return JoinPath(home, "Library/Application Support");
#else
  return JoinPath(home, ".local/share");
#endif
#endif
}

}  // namespace

bool IsSafeAppId(const std::string& id) {
  if (id.empty() || id == "." || id == "..")
    return false;
  for (char c : id) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok)
      return false;
  }
  return true;
}

bool IsAbsolutePath(const std::string& path) {
  if (path.empty() || path.find('\0') != std::string::npos)
    return false;
#ifdef _WIN32
  // Drive-absolute ("C:\x", "C:/x") or UNC ("\\server\share").
  if (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                           (path[0] >= 'a' && path[0] <= 'z'))) {
    if (path[1] == ':' && IsSeparator(path[2]))
      return true;
  }
  return path.size() >= 3 && path[0] == '\\' && path[1] == '\\';
#else
  return path[0] == '/';
#endif
}

std::string JoinPath(const std::string& base, const std::string& child) {
  if (base.empty())
    return child;
  if (IsSeparator(base.back()))
    return base + child;
#ifdef _WIN32
  return base + "\\" + child;
#else
  return base + "/" + child;
#endif
}

std::string ResolveAppDataDirFrom(const std::string& data_dir_env,
                                  const std::string& app_id_env,
                                  const std::string& platform_base,
                                  std::vector<std::string>* warnings) {
  if (!data_dir_env.empty()) {
    if (IsAbsolutePath(data_dir_env))
      return StripTrailingSeparators(data_dir_env);
    if (warnings) {
      warnings->push_back(
          "LAUFEY_DATA_DIR must be an absolute path; ignoring \"" +
          data_dir_env + "\"");
    }
    // Fall through: an invalid LAUFEY_DATA_DIR still leaves LAUFEY_APP_ID.
  }
  if (app_id_env.empty())
    return std::string();
  if (!IsSafeAppId(app_id_env)) {
    if (warnings) {
      warnings->push_back("LAUFEY_APP_ID \"" + app_id_env +
                          "\" is not a safe directory name (allowed: A-Z a-z "
                          "0-9 . _ -); web data will not be persisted per app");
    }
    return std::string();
  }
  if (!IsAbsolutePath(platform_base)) {
    if (warnings) {
      warnings->push_back(
          "could not determine the per-user app data directory; web data "
          "will not be persisted per app");
    }
    return std::string();
  }
  return JoinPath(platform_base, app_id_env);
}

const std::string& AppDataDir() {
  static const std::string dir = [] {
    std::string data_dir = GetEnvUtf8("LAUFEY_DATA_DIR");
    std::string app_id = GetEnvUtf8("LAUFEY_APP_ID");
    std::string base;
    if (!app_id.empty())
      base = PlatformAppDataBase();
    std::vector<std::string> warnings;
    std::string resolved =
        ResolveAppDataDirFrom(data_dir, app_id, base, &warnings);
    for (const std::string& warning : warnings)
      std::cerr << "laufey: " << warning << std::endl;
    return resolved;
  }();
  return dir;
}

bool EnsureDirectory(const std::string& path) {
  if (!IsAbsolutePath(path))
    return false;
  // Create each missing component in turn (mkdir -p). Components that already
  // exist are left as they are; new ones are owner-only on Unix. On Windows
  // they inherit the parent's ACL (%LOCALAPPDATA% is already per-user).
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && !IsSeparator(path[i]))
      continue;
    std::string part = path.substr(0, i);
#ifdef _WIN32
    if (part.size() <= 3 && part.size() >= 2 && part[1] == ':')
      continue;  // drive root
    std::wstring wpart = Utf8ToWide(part);
    DWORD attrs = GetFileAttributesW(wpart.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
      if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return false;
      continue;
    }
    if (!CreateDirectoryW(wpart.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      // UNC "\\server" / "\\server\share" prefixes can't be created; skip
      // them and let a later component report the real failure.
      if (part.size() >= 2 && part[0] == '\\' && part[1] == '\\')
        continue;
      return false;
    }
#else
    struct stat st;
    if (stat(part.c_str(), &st) == 0) {
      if (!S_ISDIR(st.st_mode))
        return false;
      continue;
    }
    if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST)
      return false;
#endif
  }
  return true;
}

std::string AppDataSubdir(const char* name) {
  const std::string& dir = AppDataDir();
  if (dir.empty())
    return std::string();
  std::string path = JoinPath(dir, name);
  if (!EnsureDirectory(path)) {
    std::cerr << "laufey: could not create web data directory \"" << path
              << "\"; web data will not be persisted per app" << std::endl;
    return std::string();
  }
  return path;
}

}  // namespace laufey_common
