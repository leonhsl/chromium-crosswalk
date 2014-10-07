// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ENHANCED_BOOKMARKS_ENHANCED_BOOKMARK_MODEL_FACTORY_H_
#define CHROME_BROWSER_ENHANCED_BOOKMARKS_ENHANCED_BOOKMARK_MODEL_FACTORY_H_

#include "base/macros.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"

template <typename T>
struct DefaultSingletonTraits;

namespace enhanced_bookmarks {

class EnhancedBookmarkModel;

// A factory to create one unique EnhancedBookmarkModel.
class EnhancedBookmarkModelFactory : public BrowserContextKeyedServiceFactory {
 public:
  static EnhancedBookmarkModelFactory* GetInstance();
  static EnhancedBookmarkModel* GetForBrowserContext(
      content::BrowserContext* context);

 private:
  friend struct DefaultSingletonTraits<EnhancedBookmarkModelFactory>;

  EnhancedBookmarkModelFactory();
  virtual ~EnhancedBookmarkModelFactory() {}

  virtual KeyedService* BuildServiceInstanceFor(
      content::BrowserContext* context) const override;

  virtual content::BrowserContext* GetBrowserContextToUse(
      content::BrowserContext* context) const override;

  DISALLOW_COPY_AND_ASSIGN(EnhancedBookmarkModelFactory);
};

}  // namespace enhanced_bookmarks

#endif  // CHROME_BROWSER_ENHANCED_BOOKMARKS_ENHANCED_BOOKMARK_MODEL_FACTORY_H_
