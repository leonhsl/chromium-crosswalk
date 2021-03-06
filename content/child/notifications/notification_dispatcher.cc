// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/notifications/notification_dispatcher.h"

#include "content/child/notifications/notification_manager.h"
#include "content/common/platform_notification_messages.h"

namespace content {

NotificationDispatcher::NotificationDispatcher(
    ThreadSafeSender* thread_safe_sender)
    : WorkerThreadMessageFilter(thread_safe_sender), next_notification_id_(0) {
}

NotificationDispatcher::~NotificationDispatcher() {}

int NotificationDispatcher::GenerateNotificationId(int thread_id) {
  base::AutoLock lock(notification_id_map_lock_);
  notification_id_map_[next_notification_id_] = thread_id;
  return next_notification_id_++;
}

bool NotificationDispatcher::ShouldHandleMessage(
    const IPC::Message& msg) const {
  return IPC_MESSAGE_CLASS(msg) == PlatformNotificationMsgStart;
}

void NotificationDispatcher::OnFilteredMessageReceived(
    const IPC::Message& msg) {
  NotificationManager::ThreadSpecificInstance(thread_safe_sender(),
                                              main_thread_task_runner(),
                                              this)->OnMessageReceived(msg);
}

bool NotificationDispatcher::GetWorkerThreadIdForMessage(
    const IPC::Message& msg,
    int* ipc_thread_id) {
  int notification_id = -1;
  const bool success = PickleIterator(msg).ReadInt(&notification_id);
  DCHECK(success);

  base::AutoLock lock(notification_id_map_lock_);
  auto iterator = notification_id_map_.find(notification_id);
  if (iterator != notification_id_map_.end()) {
    *ipc_thread_id = iterator->second;
    return true;
  }
  return false;
}

}  // namespace content
