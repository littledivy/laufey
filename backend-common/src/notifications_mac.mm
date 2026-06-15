// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications (macOS, UNUserNotificationCenter).
//
// Uses the modern UN API (10.14+). Delivery is gated on UN authorization,
// so the permission API reports the same state that governs whether
// `show_notification` actually displays anything. UN requires the process
// to run inside a bundled .app with a CFBundleIdentifier; when unbundled
// or unauthorized, `addNotificationRequest` fails and we emit a synthetic
// CLOSED event so callers see the lifecycle close out.

#include "laufey_backend_common.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace laufey_common {

namespace {

struct UnNotifEntry {
  // UN identifies requests by string. We key our own map by nid but also
  // need the identifier to look up entries from delegate callbacks (which
  // only know the request).
  std::string identifier;
  laufey_notification_event_fn on_event;
  void* user_data;
  std::vector<std::string> action_ids;
};

// All access happens on the main queue (delegate callbacks below hop
// there before touching these maps), so no mutex needed.
std::map<uint32_t, UnNotifEntry>& UnNotifMap() {
  static std::map<uint32_t, UnNotifEntry> map;
  return map;
}

std::map<std::string, uint32_t>& UnIdentToNid() {
  static std::map<std::string, uint32_t> map;
  return map;
}

// UN requires categories to be pre-registered before content tagged with
// that category id can be delivered. We accumulate one category per
// unique action-list shape (keyed by the joined id/title pairs) and
// re-set the full set whenever a new shape appears.
std::map<std::string, UNNotificationCategory*>& UnCategories() {
  static std::map<std::string, UNNotificationCategory*> map;
  return map;
}

std::atomic<uint32_t> g_next_notif_id_mac{1};

}  // namespace
}  // namespace laufey_common

@interface LaufeyUnDelegate : NSObject <UNUserNotificationCenterDelegate>
+ (instancetype)shared;
@end

@implementation LaufeyUnDelegate
+ (instancetype)shared {
  static LaufeyUnDelegate* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[LaufeyUnDelegate alloc] init];
  });
  return instance;
}

// Foreground delivery: UN's default is to NOT present banners when the
// app is frontmost. Override so the user sees the notification regardless
// of activation state — matches what `new Notification(...)` does in a
// browser. Also the place we hook for SHOWN, since UN doesn't have a
// separate "did deliver" callback that fires in all activation states.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler {
  (void)center;
  NSString* ident = notification.request.identifier;
  dispatch_async(dispatch_get_main_queue(), ^{
    std::string key = [ident UTF8String];
    auto& im = laufey_common::UnIdentToNid();
    auto it = im.find(key);
    if (it == im.end()) return;
    auto& nm = laufey_common::UnNotifMap();
    auto nit = nm.find(it->second);
    if (nit != nm.end() && nit->second.on_event) {
      nit->second.on_event(nit->second.user_data, it->second,
                           LAUFEY_NOTIFICATION_SHOWN, nullptr);
    }
  });
  completionHandler(UNNotificationPresentationOptionBanner |
                    UNNotificationPresentationOptionSound |
                    UNNotificationPresentationOptionBadge);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  NSString* ident = response.notification.request.identifier;
  NSString* actId = response.actionIdentifier;
  dispatch_async(dispatch_get_main_queue(), ^{
    std::string key = [ident UTF8String];
    auto& im = laufey_common::UnIdentToNid();
    auto it = im.find(key);
    if (it == im.end()) return;
    auto& nm = laufey_common::UnNotifMap();
    auto nit = nm.find(it->second);
    if (nit == nm.end() || !nit->second.on_event) return;
    if ([actId isEqualToString:UNNotificationDefaultActionIdentifier]) {
      // The user clicked the notification body (not an action button).
      nit->second.on_event(nit->second.user_data, it->second,
                           LAUFEY_NOTIFICATION_CLICKED, nullptr);
    } else if ([actId
                   isEqualToString:UNNotificationDismissActionIdentifier]) {
      // User explicitly dismissed (Close button on the banner). Requires
      // the category to opt in via UNNotificationCategoryOptionCustomDismissAction.
      nit->second.on_event(nit->second.user_data, it->second,
                           LAUFEY_NOTIFICATION_CLOSED, nullptr);
    } else {
      std::string aid = [actId UTF8String];
      nit->second.on_event(nit->second.user_data, it->second,
                           LAUFEY_NOTIFICATION_ACTION, aid.c_str());
    }
  });
  completionHandler();
}
@end

namespace laufey_common {

namespace {

void EnsureUnDelegate() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [UNUserNotificationCenter currentNotificationCenter].delegate =
        [LaufeyUnDelegate shared];
  });
}

// Register a category for the given action set if we haven't seen this
// shape before. Returns the category identifier to assign to content.
// Must be called on the main queue.
NSString* RegisterCategoryIfNeeded(
    const std::vector<std::pair<std::string, std::string>>& actions) {
  if (actions.empty()) return nil;
  std::string key;
  for (auto& a : actions) {
    key += a.first;
    key += '\x1f';
    key += a.second;
    key += '\x1e';
  }
  auto& cats = UnCategories();
  auto it = cats.find(key);
  if (it != cats.end()) return it->second.identifier;
  NSString* catId =
      [NSString stringWithFormat:@"laufey.cat.%lu", (unsigned long)cats.size()];
  NSMutableArray<UNNotificationAction*>* arr = [NSMutableArray array];
  for (auto& a : actions) {
    UNNotificationAction* act = [UNNotificationAction
        actionWithIdentifier:[NSString stringWithUTF8String:a.first.c_str()]
                       title:[NSString stringWithUTF8String:a.second.c_str()]
                     options:UNNotificationActionOptionForeground];
    [arr addObject:act];
  }
  UNNotificationCategory* cat = [UNNotificationCategory
        categoryWithIdentifier:catId
                       actions:arr
             intentIdentifiers:@[]
                       options:UNNotificationCategoryOptionCustomDismissAction];
  cats[key] = cat;
  NSMutableSet<UNNotificationCategory*>* all = [NSMutableSet set];
  for (auto& kv : cats) [all addObject:kv.second];
  [[UNUserNotificationCenter currentNotificationCenter]
      setNotificationCategories:all];
  return catId;
}

}  // namespace

uint32_t ShowNotificationMac(const NotificationOptions& opts,
                             laufey_notification_event_fn on_event,
                             void* user_data) {
  std::vector<std::pair<std::string, std::string>> actions;
  actions.reserve(opts.actions.size());
  for (auto& a : opts.actions) {
    actions.emplace_back(a.id, a.title);
  }

  uint32_t nid = g_next_notif_id_mac.fetch_add(1, std::memory_order_relaxed);

  // UN identifies requests by string. Tag (if given) acts as the
  // identifier so `add` with the same tag replaces the live notification
  // — matches the Web Notifications "tag" semantics. Without a tag we
  // synthesize a unique id from our nid.
  std::string identifier =
      opts.tag.empty() ? std::string("laufey.notif.") + std::to_string(nid)
                       : opts.tag;

  NSString* nsTitle = [NSString stringWithUTF8String:opts.title.c_str()];
  NSString* nsBody = [NSString stringWithUTF8String:opts.body.c_str()];
  NSString* nsIdent = [NSString stringWithUTF8String:identifier.c_str()];

  std::vector<std::string> action_ids;
  action_ids.reserve(actions.size());
  for (auto& a : actions) action_ids.push_back(a.first);

  bool silent = opts.silent;

  dispatch_async(dispatch_get_main_queue(), ^{
    EnsureUnDelegate();
    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];

    // If we're reusing a tag, the old nid->entry mapping is stale.
    // UN will replace the delivered notification automatically, but our
    // bookkeeping needs a fresh nid keyed off the same identifier.
    auto& im = UnIdentToNid();
    auto prev = im.find(identifier);
    if (prev != im.end()) {
      UnNotifMap().erase(prev->second);
      im.erase(prev);
    }

    NSString* catId = RegisterCategoryIfNeeded(actions);

    UNMutableNotificationContent* content =
        [[UNMutableNotificationContent alloc] init];
    content.title = nsTitle;
    content.body = nsBody;
    if (!silent) content.sound = [UNNotificationSound defaultSound];
    if (catId) content.categoryIdentifier = catId;

    UNNotificationRequest* req =
        [UNNotificationRequest requestWithIdentifier:nsIdent
                                             content:content
                                             trigger:nil];

    UnNotifEntry entry = {};
    entry.identifier = identifier;
    entry.on_event = on_event;
    entry.user_data = user_data;
    entry.action_ids = action_ids;
    UnNotifMap()[nid] = entry;
    UnIdentToNid()[identifier] = nid;

    [center addNotificationRequest:req
             withCompletionHandler:^(NSError* error) {
               if (!error) return;
               // Most common failures: process not bundled, or
               // authorizationStatus != authorized. There's no spec
               // event for "never showed", so we collapse it into
               // CLOSED — the JS Notification spec also fires
               // "error" + "close" in that order, but the laufey ABI
               // only has CLOSED in this list.
               dispatch_async(dispatch_get_main_queue(), ^{
                 auto& nm = UnNotifMap();
                 auto nit = nm.find(nid);
                 if (nit == nm.end()) return;
                 laufey_notification_event_fn cb = nit->second.on_event;
                 void* ud = nit->second.user_data;
                 UnIdentToNid().erase(nit->second.identifier);
                 nm.erase(nit);
                 if (cb) cb(ud, nid, LAUFEY_NOTIFICATION_CLOSED, nullptr);
               });
             }];
  });

  return nid;
}

void CloseNotificationMac(uint32_t notification_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& nm = UnNotifMap();
    auto it = nm.find(notification_id);
    if (it == nm.end()) return;
    std::string ident = it->second.identifier;
    laufey_notification_event_fn cb = it->second.on_event;
    void* ud = it->second.user_data;
    NSString* nsIdent = [NSString stringWithUTF8String:ident.c_str()];
    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];
    [center removeDeliveredNotificationsWithIdentifiers:@[ nsIdent ]];
    [center removePendingNotificationRequestsWithIdentifiers:@[ nsIdent ]];
    UnIdentToNid().erase(ident);
    nm.erase(it);
    if (cb) cb(ud, notification_id, LAUFEY_NOTIFICATION_CLOSED, nullptr);
  });
}

}  // namespace laufey_common
