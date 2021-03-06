// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_HISTORY_CONTENT_VISIT_DELEGATE_H_
#define CHROME_BROWSER_HISTORY_CONTENT_VISIT_DELEGATE_H_

#include <vector>

#include "base/macros.h"
#include "base/memory/scoped_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "components/history/core/browser/visit_delegate.h"
#include "components/visitedlink/browser/visitedlink_delegate.h"

namespace content {
class BrowserContext;
}

namespace visitedlink {
class VisitedLinkMaster;
}

// ContentVisitDelegate bridge history::VisitDelegate events to
// visitedlink::VisitedLinkMaster.
class ContentVisitDelegate : public history::VisitDelegate,
                             public visitedlink::VisitedLinkDelegate {
 public:
  explicit ContentVisitDelegate(content::BrowserContext* browser_context);
  ~ContentVisitDelegate() override;

 private:
  // Implementation of history::VisitDelegate.
  bool Init(HistoryService* history_service) override;
  void AddURL(const GURL& url) override;
  void AddURLs(const std::vector<GURL>& urls) override;
  void DeleteURLs(const std::vector<GURL>& urls) override;
  void DeleteAllURLs() override;

  // Implementation of visitedlink::VisitedLinkDelegate.
  void RebuildTable(const scoped_refptr<
      visitedlink::VisitedLinkDelegate::URLEnumerator>& enumerator) override;

  HistoryService* history_service_;  // Weak.
  scoped_ptr<visitedlink::VisitedLinkMaster> visitedlink_master_;
  base::CancelableTaskTracker task_tracker_;

  DISALLOW_COPY_AND_ASSIGN(ContentVisitDelegate);
};

#endif  // CHROME_BROWSER_HISTORY_CONTENT_VISIT_DELEGATE_H_
