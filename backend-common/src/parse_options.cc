// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"

namespace laufey_common {

namespace {

std::string ReadDictString(const laufey_backend_api_t* api, laufey_value_t* dict,
                           const char* key) {
  laufey_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_string(v))
    return std::string();
  size_t len = 0;
  char* s = api->value_get_string(v, &len);
  if (!s)
    return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  return out;
}

bool ReadDictBool(const laufey_backend_api_t* api, laufey_value_t* dict,
                  const char* key, bool dfl) {
  laufey_value_t* v = api->value_dict_get(dict, key);
  if (!v || !api->value_is_bool(v))
    return dfl;
  return api->value_get_bool(v);
}

}  // namespace

NotificationOptions ParseNotificationOptions(laufey_value_t* options,
                                             const laufey_backend_api_t* api) {
  NotificationOptions opts;
  if (!options)
    return opts;
  if (!api->value_is_dict(options)) {
    api->value_free(options);
    return opts;
  }

  opts.title = ReadDictString(api, options, "title");
  opts.body = ReadDictString(api, options, "body");
  opts.tag = ReadDictString(api, options, "tag");
  opts.silent = ReadDictBool(api, options, "silent", false);
  opts.require_interaction =
      ReadDictBool(api, options, "require_interaction", false);

  laufey_value_t* actions_val = api->value_dict_get(options, "actions");
  if (actions_val && api->value_is_list(actions_val)) {
    size_t n = api->value_list_size(actions_val);
    opts.actions.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      laufey_value_t* a = api->value_list_get(actions_val, i);
      if (!a || !api->value_is_dict(a))
        continue;
      NotificationAction act;
      act.id = ReadDictString(api, a, "id");
      act.title = ReadDictString(api, a, "title");
      if (!act.id.empty() && !act.title.empty())
        opts.actions.push_back(std::move(act));
    }
  }

  laufey_value_t* icon_val = api->value_dict_get(options, "icon");
  if (icon_val && api->value_is_binary(icon_val)) {
    size_t len = 0;
    const void* ptr = api->value_get_binary(icon_val, &len);
    if (ptr && len > 0) {
      const uint8_t* p = static_cast<const uint8_t*>(ptr);
      opts.icon_png.assign(p, p + len);
    }
  }

  api->value_free(options);
  return opts;
}

}  // namespace laufey_common
