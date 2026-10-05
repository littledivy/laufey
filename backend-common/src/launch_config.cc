// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Launch configuration (laufey-launch.json). See laufey_launch_config.h and
// docs/launch-config.md.

#include "laufey_launch_config.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "laufey_backend_common.h"
#else
#include <climits>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace laufey_common {

const char kLaunchConfigFileName[] = "laufey-launch.json";

namespace {

// A launch file is a few hundred bytes; anything this large is not one.
constexpr size_t kMaxLaunchConfigBytes = 64 * 1024;
// Nesting limit for the JSON reader (the schema needs 2).
constexpr int kMaxJsonDepth = 32;

// --- Minimal JSON reader ----------------------------------------------------
//
// RFC 8259 syntax: objects, arrays, strings (all escapes, \u surrogate pairs
// decoded to UTF-8), numbers, true/false/null. Values the schema doesn't use
// are validated for syntax and dropped.

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;
};

const char* TypeName(JsonValue::Type type) {
  switch (type) {
    case JsonValue::Type::kNull:
      return "null";
    case JsonValue::Type::kBool:
      return "a boolean";
    case JsonValue::Type::kNumber:
      return "a number";
    case JsonValue::Type::kString:
      return "a string";
    case JsonValue::Type::kArray:
      return "an array";
    case JsonValue::Type::kObject:
      return "an object";
  }
  return "?";
}

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  // Parses the whole text as one value. On failure returns false and sets
  // `error`.
  bool ParseDocument(JsonValue* out, std::string* error) {
    SkipSpace();
    if (!ParseValue(out, 0)) {
      *error = error_;
      return false;
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      *error = At("unexpected trailing characters");
      return false;
    }
    return true;
  }

 private:
  std::string At(const std::string& what) const {
    return what + " at byte " + std::to_string(pos_);
  }

  bool Fail(const std::string& what) {
    if (error_.empty())
      error_ = At(what);
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r'))
      ++pos_;
  }

  bool Consume(const char* literal) {
    size_t n = std::strlen(literal);
    if (text_.compare(pos_, n, literal) != 0)
      return false;
    pos_ += n;
    return true;
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxJsonDepth)
      return Fail("nesting too deep");
    if (pos_ >= text_.size())
      return Fail("unexpected end of input");
    char c = text_[pos_];
    if (c == '{')
      return ParseObject(out, depth);
    if (c == '[')
      return ParseArray(out, depth);
    if (c == '"') {
      out->type = JsonValue::Type::kString;
      return ParseString(&out->string);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      out->type = JsonValue::Type::kNumber;
      return ParseNumber();
    }
    if (Consume("true") || Consume("false")) {
      out->type = JsonValue::Type::kBool;
      return true;
    }
    if (Consume("null")) {
      out->type = JsonValue::Type::kNull;
      return true;
    }
    return Fail("unexpected character");
  }

  bool ParseObject(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kObject;
    ++pos_;  // '{'
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"')
        return Fail("expected a string key");
      std::string key;
      if (!ParseString(&key))
        return false;
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':')
        return Fail("expected ':'");
      ++pos_;
      SkipSpace();
      JsonValue value;
      if (!ParseValue(&value, depth + 1))
        return false;
      out->object.emplace_back(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(JsonValue* out, int depth) {
    out->type = JsonValue::Type::kArray;
    ++pos_;  // '['
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipSpace();
      JsonValue value;
      if (!ParseValue(&value, depth + 1))
        return false;
      out->array.push_back(std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseHex4(unsigned* out) {
    if (text_.size() - pos_ < 4)
      return Fail("truncated \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<unsigned>(c - 'A' + 10);
      else
        return Fail("invalid \\u escape");
    }
    *out = v;
    return true;
  }

  static void AppendUtf8(std::string* out, unsigned cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string* out) {
    ++pos_;  // opening quote
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return Fail("control character in string");
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size())
        break;
      char e = text_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out->push_back(e);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          unsigned cp = 0;
          if (!ParseHex4(&cp))
            return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned low = 0;
            if (!Consume("\\u") || !ParseHex4(&low) || low < 0xDC00 ||
                low > 0xDFFF)
              return Fail("unpaired surrogate in \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate in \\u escape");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("invalid escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber() {
    if (text_[pos_] == '-')
      ++pos_;
    auto digit = [&] {
      return pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9';
    };
    if (!digit())
      return Fail("invalid number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      while (digit())
        ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (!digit())
        return Fail("invalid number");
      while (digit())
        ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
        ++pos_;
      if (!digit())
        return Fail("invalid number");
      while (digit())
        ++pos_;
    }
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

// --- Schema -----------------------------------------------------------------

void Warn(std::vector<std::string>* warnings, const std::string& message) {
  if (warnings)
    warnings->push_back(message);
}

// A string value the schema accepts: non-empty, no NUL.
bool CheckString(const std::string& key, const JsonValue& value,
                 std::vector<std::string>* warnings) {
  if (value.type != JsonValue::Type::kString) {
    Warn(warnings, "\"" + key + "\" must be a string, not " +
                       TypeName(value.type) + "; ignoring it");
    return false;
  }
  if (value.string.empty() || value.string.find('\0') != std::string::npos) {
    Warn(warnings, "\"" + key +
                       "\" must be a non-empty string without NUL "
                       "characters; ignoring it");
    return false;
  }
  return true;
}

std::string GetEnv(const char* name) {
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
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// Everything before the last '/' of `path` ("" if there is none).
std::string ParentOf(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool EndsWith(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads the launch file at `path` into `text`. Returns false (silently) if it
// doesn't exist, false with a warning if it can't be read or is too large.
bool ReadLaunchFile(const std::string& path, std::string* text,
                    std::vector<std::string>* warnings) {
#ifdef _WIN32
  FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
  FILE* f = std::fopen(path.c_str(), "rb");
#endif
  if (!f) {
    if (errno != ENOENT && errno != ENOTDIR)
      Warn(warnings, std::string("could not open it: ") + std::strerror(errno));
    return false;
  }
  char buf[4096];
  size_t n;
  bool too_large = false;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    text->append(buf, n);
    if (text->size() > kMaxLaunchConfigBytes) {
      too_large = true;
      break;
    }
  }
  bool read_error = std::ferror(f) != 0;
  std::fclose(f);
  if (too_large) {
    Warn(warnings, "larger than " + std::to_string(kMaxLaunchConfigBytes) +
                       " bytes; ignoring the file");
    return false;
  }
  if (read_error) {
    Warn(warnings, "read error; ignoring the file");
    return false;
  }
  return true;
}

// Reports `warning` on stderr the first time `reported` is still clear: an
// ignored environment variable is worth one line per process, not one per
// read.
void ReportOnce(std::atomic<bool>* reported, const std::string& warning) {
  if (warning.empty() || reported->exchange(true))
    return;
  std::cerr << "laufey: " << warning << std::endl;
}

// The warning for an environment variable a pinned app id overrides.
std::string PinnedEnvWarning(const char* env_name) {
  return std::string(env_name) +
         " is ignored: the app's launch file pins its app id";
}

LaunchConfig LoadProcessLaunchConfig() {
  std::string path = LaunchConfigPathForExecutable(ExecutablePath());
  if (path.empty())
    return LaunchConfig();
  std::vector<std::string> warnings;
  std::string text;
  LaunchConfig config;
  if (ReadLaunchFile(path, &text, &warnings))
    config = ParseLaunchConfig(text, &warnings);
  for (const std::string& warning : warnings)
    std::cerr << "laufey: " << path << ": " << warning << std::endl;
  return config;
}

}  // namespace

LaunchConfig ParseLaunchConfig(const std::string& text,
                               std::vector<std::string>* warnings) {
  LaunchConfig config;
  // Tolerate a UTF-8 byte order mark (some Windows editors write one).
  std::string body = text;
  if (body.compare(0, 3, "\xEF\xBB\xBF") == 0)
    body.erase(0, 3);
  JsonValue root;
  std::string error;
  if (!JsonReader(body).ParseDocument(&root, &error)) {
    Warn(warnings, "not valid JSON (" + error + "); ignoring the file");
    return config;
  }
  if (root.type != JsonValue::Type::kObject) {
    Warn(warnings, std::string("the top level must be an object, not ") +
                       TypeName(root.type) + "; ignoring the file");
    return config;
  }
  std::vector<std::string> seen;
  for (const auto& [key, value] : root.object) {
    bool duplicate = false;
    for (const std::string& k : seen)
      duplicate = duplicate || k == key;
    if (duplicate)
      Warn(warnings, "duplicate key \"" + key + "\"; the last one wins");
    else
      seen.push_back(key);
    if (key == "appId") {
      config.has_app_id = false;
      if (CheckString(key, value, warnings)) {
        config.has_app_id = true;
        config.app_id = value.string;
      }
    } else if (key == "dataDir") {
      config.has_data_dir = false;
      if (CheckString(key, value, warnings)) {
        config.has_data_dir = true;
        config.data_dir = value.string;
      }
    } else if (key == "customSchemes") {
      config.has_custom_schemes = false;
      config.custom_schemes.clear();
      if (value.type != JsonValue::Type::kArray) {
        Warn(warnings, std::string("\"customSchemes\" must be an array of "
                                   "strings, not ") +
                           TypeName(value.type) + "; ignoring it");
        continue;
      }
      config.has_custom_schemes = true;
      for (const JsonValue& item : value.array) {
        if (!CheckString("customSchemes[]", item, warnings))
          continue;
        // The value stands in for the comma-separated LAUFEY_CUSTOM_SCHEMES.
        if (item.string.find(',') != std::string::npos) {
          Warn(warnings, "\"customSchemes\" entry \"" + item.string +
                             "\" contains ','; ignoring it");
          continue;
        }
        config.custom_schemes.push_back(item.string);
      }
    } else {
      Warn(warnings, "unknown key \"" + key + "\"; ignoring it");
    }
  }
  return config;
}

std::string MacBundleResourcesDir(const std::string& exe_path) {
  // <B>.app/Contents/MacOS/<exe>
  std::string macos_dir = ParentOf(exe_path);
  if (!EndsWith(macos_dir, "/Contents/MacOS"))
    return std::string();
  std::string contents = ParentOf(macos_dir);
  std::string bundle = ParentOf(contents);
  if (!EndsWith(bundle, ".app"))
    return std::string();
  // A helper app (CEF's "<App> Helper.app") lives in the main app's
  // Contents/Frameworks; the launch file belongs to the main app.
  std::string frameworks = ParentOf(bundle);
  if (EndsWith(frameworks, "/Contents/Frameworks")) {
    std::string outer = ParentOf(ParentOf(frameworks));
    if (EndsWith(outer, ".app"))
      bundle = outer;
  }
  return bundle + "/Contents/Resources";
}

std::string LaunchConfigPathForExecutable(const std::string& exe_path) {
#ifdef __APPLE__
  std::string resources = MacBundleResourcesDir(exe_path);
  if (!resources.empty())
    return resources + "/" + kLaunchConfigFileName;
#endif
  size_t end = exe_path.size();
  while (end > 0 && !IsPathSeparator(exe_path[end - 1]))
    --end;
  if (end == 0)
    return std::string();
  // Keep the separator the path already uses.
  return exe_path.substr(0, end) + kLaunchConfigFileName;
}

std::string ExecutablePath() {
#ifdef _WIN32
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n =
        GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0)
      return std::string();
    if (n < buf.size()) {
      buf.resize(n);
      return WideToUtf8(buf);
    }
    if (buf.size() >= 32768)
      return std::string();
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string raw(size, '\0');
  if (_NSGetExecutablePath(&raw[0], &size) != 0)
    return std::string();
  raw.resize(std::strlen(raw.c_str()));
  char resolved[PATH_MAX];
  if (realpath(raw.c_str(), resolved))
    return resolved;
  return raw;
#else
  std::string buf(256, '\0');
  for (;;) {
    ssize_t n = readlink("/proc/self/exe", &buf[0], buf.size());
    if (n < 0)
      return std::string();
    if (static_cast<size_t>(n) < buf.size()) {
      buf.resize(static_cast<size_t>(n));
      return buf;
    }
    if (buf.size() >= 65536)
      return std::string();
    buf.resize(buf.size() * 2);
  }
#endif
}

const LaunchConfig& ProcessLaunchConfig() {
  static const LaunchConfig config = LoadProcessLaunchConfig();
  return config;
}

std::string LaunchSettingFrom(const std::string& env_value, bool file_has,
                              const std::string& file_value) {
  if (!env_value.empty())
    return env_value;
  return file_has ? file_value : std::string();
}

std::string LaunchPinnedSettingFrom(const std::string& env_value, bool file_has,
                                    const std::string& file_value) {
  if (file_has)
    return file_value;
  return env_value;
}

std::string LaunchAppId() {
  const LaunchConfig& file = ProcessLaunchConfig();
  return LaunchPinnedSettingFrom(GetEnv("LAUFEY_APP_ID"), file.has_app_id,
                                 file.app_id);
}

std::string LaunchDataDirFrom(const std::string& env_value, bool app_id_pinned,
                              bool file_has, const std::string& file_value,
                              std::string* warning) {
  if (file_has || app_id_pinned) {
    if (warning && !env_value.empty()) {
      *warning = file_has ? std::string(
                                "LAUFEY_DATA_DIR is ignored: the app's launch "
                                "file sets its dataDir")
                          : PinnedEnvWarning("LAUFEY_DATA_DIR");
    }
    return file_has ? file_value : std::string();
  }
  return env_value;
}

std::string LaunchCustomSchemesFrom(const std::string& env_value,
                                    bool app_id_pinned, bool file_has,
                                    const std::string& file_value,
                                    std::string* warning) {
  if (!app_id_pinned)
    return LaunchSettingFrom(env_value, file_has, file_value);
  if (warning && !env_value.empty())
    *warning = PinnedEnvWarning("LAUFEY_CUSTOM_SCHEMES");
  return file_has ? file_value : std::string();
}

std::string LaunchDataDir() {
  static std::atomic<bool> reported{false};
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string warning;
  std::string dir =
      LaunchDataDirFrom(GetEnv("LAUFEY_DATA_DIR"), file.has_app_id,
                        file.has_data_dir, file.data_dir, &warning);
  ReportOnce(&reported, warning);
  return dir;
}

std::string LaunchCustomSchemes() {
  static std::atomic<bool> reported{false};
  const LaunchConfig& file = ProcessLaunchConfig();
  std::string joined;
  for (const std::string& scheme : file.custom_schemes) {
    if (!joined.empty())
      joined.push_back(',');
    joined += scheme;
  }
  std::string warning;
  std::string schemes =
      LaunchCustomSchemesFrom(GetEnv("LAUFEY_CUSTOM_SCHEMES"), file.has_app_id,
                              file.has_custom_schemes, joined, &warning);
  ReportOnce(&reported, warning);
  return schemes;
}

}  // namespace laufey_common
