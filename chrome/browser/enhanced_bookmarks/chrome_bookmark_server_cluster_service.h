// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ENHANCED_BOOKMARKS_CHROME_BOOKMARK_SERVER_CLUSTER_SERVICE_H_
#define CHROME_BROWSER_ENHANCED_BOOKMARKS_CHROME_BOOKMARK_SERVER_CLUSTER_SERVICE_H_

#include "chrome/browser/sync/profile_sync_service_observer.h"
#include "components/enhanced_bookmarks/bookmark_server_cluster_service.h"

class ProfileSyncService;

namespace enhanced_bookmarks {

// A cluster service that invalidates its data when a sync operation finishes.
class ChromeBookmarkServerClusterService : public BookmarkServerClusterService,
                                           public ProfileSyncServiceObserver {
 public:
  ChromeBookmarkServerClusterService(
      const std::string& application_language_code,
      scoped_refptr<net::URLRequestContextGetter> request_context_getter,
      ProfileOAuth2TokenService* token_service,
      SigninManagerBase* signin_manager,
      EnhancedBookmarkModel* enhanced_bookmark_model,
      PrefService* pref_service,
      ProfileSyncService* sync_service);
  virtual ~ChromeBookmarkServerClusterService();

  // ProfileSyncServiceObserver implementation.
  virtual void OnStateChanged() override;
  virtual void OnSyncCycleCompleted() override;

 private:
  // This class observes the sync service for changes.
  ProfileSyncService* sync_service_;

  DISALLOW_COPY_AND_ASSIGN(ChromeBookmarkServerClusterService);
};

}  // namespace enhanced_bookmarks

#endif  // CHROME_BROWSER_ENHANCED_BOOKMARKS_CHROME_BOOKMARK_SERVER_CLUSTER_SERVICE_H_
