// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/declarative_user_script_manager.h"

#include "chrome/browser/extensions/declarative_user_script_master.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/browser/extension_registry.h"

namespace extensions {

DeclarativeUserScriptManager::DeclarativeUserScriptManager(Profile* profile)
    : profile_(profile), extension_registry_observer_(this) {
  extension_registry_observer_.Add(ExtensionRegistry::Get(profile));
}

DeclarativeUserScriptManager::~DeclarativeUserScriptManager() {
}

DeclarativeUserScriptMaster*
DeclarativeUserScriptManager::GetDeclarativeUserScriptMasterByID(
    const HostID& host_id) {
  UserScriptMasterMap::iterator it =
      declarative_user_script_masters_.find(host_id);

  if (it != declarative_user_script_masters_.end())
    return it->second.get();

  return CreateDeclarativeUserScriptMaster(host_id);
}

DeclarativeUserScriptMaster*
DeclarativeUserScriptManager::CreateDeclarativeUserScriptMaster(
    const HostID& host_id) {
  linked_ptr<DeclarativeUserScriptMaster> master(
      new DeclarativeUserScriptMaster(profile_, host_id));
  declarative_user_script_masters_[host_id] = master;
  return master.get();
}

void DeclarativeUserScriptManager::OnExtensionUnloaded(
    content::BrowserContext* browser_context,
    const Extension* extension,
    UnloadedExtensionInfo::Reason reason) {
  for (const auto& val : declarative_user_script_masters_) {
    DeclarativeUserScriptMaster* master = val.second.get();
    if (master->host_id().id() == extension->id())
      master->ClearScripts();
  }
}

}  // namespace extensions
