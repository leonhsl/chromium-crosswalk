// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_version.h"

#include "base/command_line.h"
#include "base/memory/ref_counted.h"
#include "base/stl_util.h"
#include "base/strings/string16.h"
#include "content/browser/message_port_message_filter.h"
#include "content/browser/message_port_service.h"
#include "content/browser/service_worker/embedded_worker_instance.h"
#include "content/browser/service_worker/embedded_worker_registry.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_utils.h"
#include "content/common/service_worker/service_worker_messages.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_switches.h"

namespace content {

typedef ServiceWorkerVersion::StatusCallback StatusCallback;
typedef ServiceWorkerVersion::MessageCallback MessageCallback;

class ServiceWorkerVersion::GetClientDocumentsCallback
    : public base::RefCounted<GetClientDocumentsCallback> {
 public:
  GetClientDocumentsCallback(int request_id, int pending_requests)
      : request_id_(request_id),
        pending_requests_(pending_requests) {}
  void AddClientInfo(int client_id, const ServiceWorkerClientInfo& info) {
    clients_.push_back(info);
    clients_.back().client_id = client_id;
  }
  void DecrementPendingRequests(ServiceWorkerVersion* version) {
    if (--pending_requests_ > 0)
      return;
    // Don't bother if it's no longer running.
    if (version->running_status() == RUNNING) {
      version->embedded_worker_->SendMessage(
          ServiceWorkerMsg_DidGetClientDocuments(request_id_, clients_));
    }
  }

 private:
  friend class base::RefCounted<GetClientDocumentsCallback>;
  virtual ~GetClientDocumentsCallback() {}

  std::vector<ServiceWorkerClientInfo> clients_;
  int request_id_;
  size_t pending_requests_;

  DISALLOW_COPY_AND_ASSIGN(GetClientDocumentsCallback);
};

class ServiceWorkerVersion::GetClientInfoCallback {
 public:
  GetClientInfoCallback(
      int client_id,
      const scoped_refptr<GetClientDocumentsCallback>& callback)
      : client_id_(client_id),
        callback_(callback) {}

  void OnSuccess(ServiceWorkerVersion* version,
                 const ServiceWorkerClientInfo& info) {
    callback_->AddClientInfo(client_id_, info);
    callback_->DecrementPendingRequests(version);
  }
  void OnError(ServiceWorkerVersion* version) {
    callback_->DecrementPendingRequests(version);
  }

 private:
  int client_id_;
  scoped_refptr<GetClientDocumentsCallback> callback_;

  DISALLOW_COPY_AND_ASSIGN(GetClientInfoCallback);
};

namespace {

// Default delay for scheduled stop.
// (Note that if all references to the version is dropped the worker
// is also stopped without delay)
const int64 kStopWorkerDelay = 30;  // 30 secs.

// Default delay for scheduled update.
const int kUpdateDelaySeconds = 1;

void RunSoon(const base::Closure& callback) {
  if (!callback.is_null())
    base::MessageLoop::current()->PostTask(FROM_HERE, callback);
}

template <typename CallbackArray, typename Arg>
void RunCallbacks(ServiceWorkerVersion* version,
                  CallbackArray* callbacks_ptr,
                  const Arg& arg) {
  CallbackArray callbacks;
  callbacks.swap(*callbacks_ptr);
  scoped_refptr<ServiceWorkerVersion> protect(version);
  for (const auto& callback : callbacks)
    callback.Run(arg);
}

template <typename IDMAP, typename... Params>
void RunIDMapCallbacks(IDMAP* callbacks, const Params&... params) {
  typename IDMAP::iterator iter(callbacks);
  while (!iter.IsAtEnd()) {
    iter.GetCurrentValue()->Run(params...);
    iter.Advance();
  }
  callbacks->Clear();
}

// A callback adapter to start a |task| after StartWorker.
void RunTaskAfterStartWorker(
    base::WeakPtr<ServiceWorkerVersion> version,
    const StatusCallback& error_callback,
    const base::Closure& task,
    ServiceWorkerStatusCode status) {
  if (status != SERVICE_WORKER_OK) {
    if (!error_callback.is_null())
      error_callback.Run(status);
    return;
  }
  if (version->running_status() != ServiceWorkerVersion::RUNNING) {
    // We've tried to start the worker (and it has succeeded), but
    // it looks it's not running yet.
    NOTREACHED() << "The worker's not running after successful StartWorker";
    if (!error_callback.is_null())
      error_callback.Run(SERVICE_WORKER_ERROR_START_WORKER_FAILED);
    return;
  }
  task.Run();
}

void RunErrorFetchCallback(const ServiceWorkerVersion::FetchCallback& callback,
                           ServiceWorkerStatusCode status) {
  callback.Run(status,
               SERVICE_WORKER_FETCH_EVENT_RESULT_FALLBACK,
               ServiceWorkerResponse());
}

void RunErrorMessageCallback(
    const std::vector<int>& sent_message_port_ids,
    const ServiceWorkerVersion::StatusCallback& callback,
    ServiceWorkerStatusCode status) {
  // Transfering the message ports failed, so destroy the ports.
  for (int message_port_id : sent_message_port_ids) {
    MessagePortService::GetInstance()->ClosePort(message_port_id);
  }
  callback.Run(status);
}

void RunErrorCrossOriginConnectCallback(
    const ServiceWorkerVersion::CrossOriginConnectCallback& callback,
    ServiceWorkerStatusCode status) {
  callback.Run(status, false);
}

}  // namespace

ServiceWorkerVersion::ServiceWorkerVersion(
    ServiceWorkerRegistration* registration,
    const GURL& script_url,
    int64 version_id,
    base::WeakPtr<ServiceWorkerContextCore> context)
    : version_id_(version_id),
      registration_id_(kInvalidServiceWorkerVersionId),
      script_url_(script_url),
      status_(NEW),
      context_(context),
      script_cache_map_(this, context),
      is_doomed_(false),
      skip_waiting_(false),
      weak_factory_(this) {
  DCHECK(context_);
  DCHECK(registration);
  if (registration) {
    registration_id_ = registration->id();
    scope_ = registration->pattern();
  }
  context_->AddLiveVersion(this);
  embedded_worker_ = context_->embedded_worker_registry()->CreateWorker();
  embedded_worker_->AddListener(this);
}

ServiceWorkerVersion::~ServiceWorkerVersion() {
  embedded_worker_->RemoveListener(this);
  if (context_)
    context_->RemoveLiveVersion(version_id_);
  // EmbeddedWorker's dtor sends StopWorker if it's still running.
}

void ServiceWorkerVersion::SetStatus(Status status) {
  if (status_ == status)
    return;

  status_ = status;

  if (skip_waiting_ && status_ == ACTIVATED) {
    for (int request_id : pending_skip_waiting_requests_)
      DidSkipWaiting(request_id);
    pending_skip_waiting_requests_.clear();
  }

  std::vector<base::Closure> callbacks;
  callbacks.swap(status_change_callbacks_);
  for (const auto& callback : callbacks)
    callback.Run();

  FOR_EACH_OBSERVER(Listener, listeners_, OnVersionStateChanged(this));
}

void ServiceWorkerVersion::RegisterStatusChangeCallback(
    const base::Closure& callback) {
  status_change_callbacks_.push_back(callback);
}

ServiceWorkerVersionInfo ServiceWorkerVersion::GetInfo() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  return ServiceWorkerVersionInfo(
      running_status(),
      status(),
      script_url(),
      version_id(),
      embedded_worker()->process_id(),
      embedded_worker()->thread_id(),
      embedded_worker()->worker_devtools_agent_route_id());
}

void ServiceWorkerVersion::StartWorker(const StatusCallback& callback) {
  StartWorker(false, callback);
}

void ServiceWorkerVersion::StartWorker(
    bool pause_after_download,
    const StatusCallback& callback) {
  if (is_doomed()) {
    RunSoon(base::Bind(callback, SERVICE_WORKER_ERROR_START_WORKER_FAILED));
    return;
  }
  switch (running_status()) {
    case RUNNING:
      RunSoon(base::Bind(callback, SERVICE_WORKER_OK));
      return;
    case STOPPING:
    case STOPPED:
    case STARTING:
      start_callbacks_.push_back(callback);
      if (running_status() == STOPPED) {
        DCHECK(!cache_listener_.get());
        cache_listener_.reset(new ServiceWorkerCacheListener(this, context_));
        embedded_worker_->Start(
            version_id_,
            scope_,
            script_url_,
            pause_after_download,
            base::Bind(&ServiceWorkerVersion::OnStartMessageSent,
                       weak_factory_.GetWeakPtr()));
      }
      return;
  }
}

void ServiceWorkerVersion::StopWorker(const StatusCallback& callback) {
  if (running_status() == STOPPED) {
    RunSoon(base::Bind(callback, SERVICE_WORKER_OK));
    return;
  }
  if (stop_callbacks_.empty()) {
    ServiceWorkerStatusCode status = embedded_worker_->Stop();
    if (status != SERVICE_WORKER_OK) {
      RunSoon(base::Bind(callback, status));
      return;
    }
  }
  stop_callbacks_.push_back(callback);
}

void ServiceWorkerVersion::ScheduleUpdate() {
  if (update_timer_.IsRunning()) {
    update_timer_.Reset();
    return;
  }
  update_timer_.Start(
      FROM_HERE, base::TimeDelta::FromSeconds(kUpdateDelaySeconds),
      base::Bind(&ServiceWorkerVersion::StartUpdate,
                 weak_factory_.GetWeakPtr()));
}

void ServiceWorkerVersion::DeferScheduledUpdate() {
  if (update_timer_.IsRunning())
    update_timer_.Reset();
}

void ServiceWorkerVersion::StartUpdate() {
  update_timer_.Stop();
  if (!context_)
    return;
  ServiceWorkerRegistration* registration =
      context_->GetLiveRegistration(registration_id_);
  if (!registration || !registration->GetNewestVersion())
    return;
  context_->UpdateServiceWorker(registration);
}

void ServiceWorkerVersion::SendMessage(
    const IPC::Message& message, const StatusCallback& callback) {
  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(), callback,
                           base::Bind(&self::SendMessage,
                                      weak_factory_.GetWeakPtr(),
                                      message, callback)));
    return;
  }

  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(message);
  RunSoon(base::Bind(callback, status));
}

void ServiceWorkerVersion::DispatchMessageEvent(
    const base::string16& message,
    const std::vector<int>& sent_message_port_ids,
    const StatusCallback& callback) {
  for (int message_port_id : sent_message_port_ids) {
    MessagePortService::GetInstance()->HoldMessages(message_port_id);
  }

  DispatchMessageEventInternal(message, sent_message_port_ids, callback);
}

void ServiceWorkerVersion::DispatchMessageEventInternal(
    const base::string16& message,
    const std::vector<int>& sent_message_port_ids,
    const StatusCallback& callback) {
  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(
        &RunTaskAfterStartWorker, weak_factory_.GetWeakPtr(),
        base::Bind(&RunErrorMessageCallback, sent_message_port_ids, callback),
        base::Bind(&self::DispatchMessageEventInternal,
                   weak_factory_.GetWeakPtr(), message, sent_message_port_ids,
                   callback)));
    return;
  }

  MessagePortMessageFilter* filter =
      embedded_worker_->message_port_message_filter();
  std::vector<int> new_routing_ids;
  filter->UpdateMessagePortsWithNewRoutes(sent_message_port_ids,
                                          &new_routing_ids);
  ServiceWorkerStatusCode status =
      embedded_worker_->SendMessage(ServiceWorkerMsg_MessageToWorker(
          message, sent_message_port_ids, new_routing_ids));
  RunSoon(base::Bind(callback, status));
}

void ServiceWorkerVersion::DispatchInstallEvent(
    int active_version_id,
    const StatusCallback& callback) {
  DCHECK_EQ(INSTALLING, status()) << status();

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(
        base::Bind(&RunTaskAfterStartWorker,
                   weak_factory_.GetWeakPtr(),
                   callback,
                   base::Bind(&self::DispatchInstallEventAfterStartWorker,
                              weak_factory_.GetWeakPtr(),
                              active_version_id,
                              callback)));
  } else {
    DispatchInstallEventAfterStartWorker(active_version_id, callback);
  }
}

void ServiceWorkerVersion::DispatchActivateEvent(
    const StatusCallback& callback) {
  DCHECK_EQ(ACTIVATING, status()) << status();

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(
        base::Bind(&RunTaskAfterStartWorker,
                   weak_factory_.GetWeakPtr(),
                   callback,
                   base::Bind(&self::DispatchActivateEventAfterStartWorker,
                              weak_factory_.GetWeakPtr(),
                              callback)));
  } else {
    DispatchActivateEventAfterStartWorker(callback);
  }
}

void ServiceWorkerVersion::DispatchFetchEvent(
    const ServiceWorkerFetchRequest& request,
    const base::Closure& prepare_callback,
    const FetchCallback& fetch_callback) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(),
                           base::Bind(&RunErrorFetchCallback, fetch_callback),
                           base::Bind(&self::DispatchFetchEvent,
                                      weak_factory_.GetWeakPtr(),
                                      request,
                                      prepare_callback,
                                      fetch_callback)));
    return;
  }

  prepare_callback.Run();

  int request_id = fetch_callbacks_.Add(new FetchCallback(fetch_callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_FetchEvent(request_id, request));
  if (status != SERVICE_WORKER_OK) {
    fetch_callbacks_.Remove(request_id);
    RunSoon(base::Bind(&RunErrorFetchCallback,
                       fetch_callback,
                       SERVICE_WORKER_ERROR_FAILED));
  }
}

void ServiceWorkerVersion::DispatchSyncEvent(const StatusCallback& callback) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableServiceWorkerSync)) {
    callback.Run(SERVICE_WORKER_ERROR_ABORT);
    return;
  }

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(), callback,
                           base::Bind(&self::DispatchSyncEvent,
                                      weak_factory_.GetWeakPtr(),
                                      callback)));
    return;
  }

  int request_id = sync_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_SyncEvent(request_id));
  if (status != SERVICE_WORKER_OK) {
    sync_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::DispatchNotificationClickEvent(
    const StatusCallback& callback,
    const std::string& notification_id,
    const PlatformNotificationData& notification_data) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExperimentalWebPlatformFeatures)) {
    callback.Run(SERVICE_WORKER_ERROR_ABORT);
    return;
  }

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(), callback,
                           base::Bind(&self::DispatchNotificationClickEvent,
                                      weak_factory_.GetWeakPtr(),
                                      callback, notification_id,
                                      notification_data)));
    return;
  }

  int request_id =
      notification_click_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_NotificationClickEvent(request_id,
                                              notification_id,
                                              notification_data));
  if (status != SERVICE_WORKER_OK) {
    notification_click_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::DispatchPushEvent(const StatusCallback& callback,
                                             const std::string& data) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExperimentalWebPlatformFeatures)) {
    callback.Run(SERVICE_WORKER_ERROR_ABORT);
    return;
  }

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(), callback,
                           base::Bind(&self::DispatchPushEvent,
                                      weak_factory_.GetWeakPtr(),
                                      callback, data)));
    return;
  }

  int request_id = push_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_PushEvent(request_id, data));
  if (status != SERVICE_WORKER_OK) {
    push_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::DispatchGeofencingEvent(
    const StatusCallback& callback,
    blink::WebGeofencingEventType event_type,
    const std::string& region_id,
    const blink::WebCircularGeofencingRegion& region) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExperimentalWebPlatformFeatures)) {
    callback.Run(SERVICE_WORKER_ERROR_ABORT);
    return;
  }

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(&RunTaskAfterStartWorker,
                           weak_factory_.GetWeakPtr(),
                           callback,
                           base::Bind(&self::DispatchGeofencingEvent,
                                      weak_factory_.GetWeakPtr(),
                                      callback,
                                      event_type,
                                      region_id,
                                      region)));
    return;
  }

  int request_id = geofencing_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status =
      embedded_worker_->SendMessage(ServiceWorkerMsg_GeofencingEvent(
          request_id, event_type, region_id, region));
  if (status != SERVICE_WORKER_OK) {
    geofencing_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::DispatchCrossOriginConnectEvent(
    const CrossOriginConnectCallback& callback,
    const CrossOriginServiceWorkerClient& client) {
  DCHECK_EQ(ACTIVATED, status()) << status();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExperimentalWebPlatformFeatures)) {
    callback.Run(SERVICE_WORKER_ERROR_ABORT, false);
    return;
  }

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(
        base::Bind(&RunTaskAfterStartWorker, weak_factory_.GetWeakPtr(),
                   base::Bind(&RunErrorCrossOriginConnectCallback, callback),
                   base::Bind(&self::DispatchCrossOriginConnectEvent,
                              weak_factory_.GetWeakPtr(), callback, client)));
    return;
  }

  int request_id = cross_origin_connect_callbacks_.Add(
      new CrossOriginConnectCallback(callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_CrossOriginConnectEvent(request_id, client));
  if (status != SERVICE_WORKER_OK) {
    cross_origin_connect_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status, false));
  }
}

void ServiceWorkerVersion::DispatchCrossOriginMessageEvent(
    const CrossOriginServiceWorkerClient& client,
    const base::string16& message,
    const std::vector<int>& sent_message_port_ids,
    const StatusCallback& callback) {
  // Unlike in the case of DispatchMessageEvent, here the caller is assumed to
  // have already put all the sent message ports on hold. So no need to do that
  // here again.

  if (running_status() != RUNNING) {
    // Schedule calling this method after starting the worker.
    StartWorker(base::Bind(
        &RunTaskAfterStartWorker, weak_factory_.GetWeakPtr(), callback,
        base::Bind(&self::DispatchCrossOriginMessageEvent,
                   weak_factory_.GetWeakPtr(), client, message,
                   sent_message_port_ids, callback)));
    return;
  }

  MessagePortMessageFilter* filter =
      embedded_worker_->message_port_message_filter();
  std::vector<int> new_routing_ids;
  filter->UpdateMessagePortsWithNewRoutes(sent_message_port_ids,
                                          &new_routing_ids);
  ServiceWorkerStatusCode status =
      embedded_worker_->SendMessage(ServiceWorkerMsg_CrossOriginMessageToWorker(
          client, message, sent_message_port_ids, new_routing_ids));
  RunSoon(base::Bind(callback, status));
}
void ServiceWorkerVersion::AddControllee(
    ServiceWorkerProviderHost* provider_host) {
  DCHECK(!ContainsKey(controllee_map_, provider_host));
  int controllee_id = controllee_by_id_.Add(provider_host);
  controllee_map_[provider_host] = controllee_id;
  // Reset the timer if it's running (so that it's kept alive a bit longer
  // right after a new controllee is added).
  ScheduleStopWorker();
}

void ServiceWorkerVersion::RemoveControllee(
    ServiceWorkerProviderHost* provider_host) {
  ControlleeMap::iterator found = controllee_map_.find(provider_host);
  DCHECK(found != controllee_map_.end());
  controllee_by_id_.Remove(found->second);
  controllee_map_.erase(found);
  if (HasControllee())
    return;
  FOR_EACH_OBSERVER(Listener, listeners_, OnNoControllees(this));
  if (is_doomed_) {
    DoomInternal();
    return;
  }
  // Schedule the stop-worker-timer if it's not running.
  if (!stop_worker_timer_.IsRunning())
    ScheduleStopWorker();
}

void ServiceWorkerVersion::AddListener(Listener* listener) {
  listeners_.AddObserver(listener);
}

void ServiceWorkerVersion::RemoveListener(Listener* listener) {
  listeners_.RemoveObserver(listener);
}

void ServiceWorkerVersion::Doom() {
  if (is_doomed_)
    return;
  is_doomed_ = true;
  if (!HasControllee())
    DoomInternal();
}

void ServiceWorkerVersion::SetDevToolsAttached(bool attached) {
  embedded_worker()->set_devtools_attached(attached);
  if (!attached && !stop_worker_timer_.IsRunning()) {
    // If devtools is detached from this version and stop-worker-timer is not
    // running, try scheduling stop-worker-timer now.
    ScheduleStopWorker();
  }
}

void ServiceWorkerVersion::OnStarted() {
  DCHECK_EQ(RUNNING, running_status());
  DCHECK(cache_listener_.get());
  ScheduleStopWorker();
  // Fire all start callbacks.
  RunCallbacks(this, &start_callbacks_, SERVICE_WORKER_OK);
  FOR_EACH_OBSERVER(Listener, listeners_, OnWorkerStarted(this));
}

void ServiceWorkerVersion::OnStopped(
    EmbeddedWorkerInstance::Status old_status) {
  DCHECK_EQ(STOPPED, running_status());
  scoped_refptr<ServiceWorkerVersion> protect(this);

  bool should_restart = !is_doomed() && !start_callbacks_.empty() &&
                        (old_status != EmbeddedWorkerInstance::STARTING);

  // Fire all stop callbacks.
  RunCallbacks(this, &stop_callbacks_, SERVICE_WORKER_OK);

  if (!should_restart) {
    // Let all start callbacks fail.
    RunCallbacks(this, &start_callbacks_,
                 SERVICE_WORKER_ERROR_START_WORKER_FAILED);
  }

  // Let all message callbacks fail (this will also fire and clear all
  // callbacks for events).
  // TODO(kinuko): Consider if we want to add queue+resend mechanism here.
  RunIDMapCallbacks(&activate_callbacks_,
                    SERVICE_WORKER_ERROR_ACTIVATE_WORKER_FAILED);
  RunIDMapCallbacks(&install_callbacks_,
                    SERVICE_WORKER_ERROR_INSTALL_WORKER_FAILED);
  RunIDMapCallbacks(&fetch_callbacks_,
                    SERVICE_WORKER_ERROR_FAILED,
                    SERVICE_WORKER_FETCH_EVENT_RESULT_FALLBACK,
                    ServiceWorkerResponse());
  RunIDMapCallbacks(&sync_callbacks_,
                    SERVICE_WORKER_ERROR_FAILED);
  RunIDMapCallbacks(&push_callbacks_,
                    SERVICE_WORKER_ERROR_FAILED);
  RunIDMapCallbacks(&geofencing_callbacks_,
                    SERVICE_WORKER_ERROR_FAILED);

  get_client_info_callbacks_.Clear();

  FOR_EACH_OBSERVER(Listener, listeners_, OnWorkerStopped(this));

  // There should be no more communication from/to a stopped worker. Deleting
  // the listener prevents any pending completion callbacks from causing
  // messages to be sent to the stopped worker.
  cache_listener_.reset();

  // Restart worker if we have any start callbacks and the worker isn't doomed.
  if (should_restart) {
    cache_listener_.reset(new ServiceWorkerCacheListener(this, context_));
    embedded_worker_->Start(
        version_id_, scope_, script_url_, false /* pause_after_download */,
        base::Bind(&ServiceWorkerVersion::OnStartMessageSent,
                   weak_factory_.GetWeakPtr()));
  }
}

void ServiceWorkerVersion::OnReportException(
    const base::string16& error_message,
    int line_number,
    int column_number,
    const GURL& source_url) {
  FOR_EACH_OBSERVER(
      Listener,
      listeners_,
      OnErrorReported(
          this, error_message, line_number, column_number, source_url));
}

void ServiceWorkerVersion::OnReportConsoleMessage(int source_identifier,
                                                  int message_level,
                                                  const base::string16& message,
                                                  int line_number,
                                                  const GURL& source_url) {
  FOR_EACH_OBSERVER(Listener,
                    listeners_,
                    OnReportConsoleMessage(this,
                                           source_identifier,
                                           message_level,
                                           message,
                                           line_number,
                                           source_url));
}

bool ServiceWorkerVersion::OnMessageReceived(const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(ServiceWorkerVersion, message)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_GetClientDocuments,
                        OnGetClientDocuments)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_ActivateEventFinished,
                        OnActivateEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_InstallEventFinished,
                        OnInstallEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_FetchEventFinished,
                        OnFetchEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_SyncEventFinished,
                        OnSyncEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_NotificationClickEventFinished,
                        OnNotificationClickEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_PushEventFinished,
                        OnPushEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_GeofencingEventFinished,
                        OnGeofencingEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_CrossOriginConnectEventFinished,
                        OnCrossOriginConnectEventFinished)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_PostMessageToDocument,
                        OnPostMessageToDocument)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_FocusClient,
                        OnFocusClient)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_GetClientInfoSuccess,
                        OnGetClientInfoSuccess)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_GetClientInfoError,
                        OnGetClientInfoError)
    IPC_MESSAGE_HANDLER(ServiceWorkerHostMsg_SkipWaiting,
                        OnSkipWaiting)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void ServiceWorkerVersion::OnStartMessageSent(
    ServiceWorkerStatusCode status) {
  if (status != SERVICE_WORKER_OK)
    RunCallbacks(this, &start_callbacks_, status);
}

void ServiceWorkerVersion::DispatchInstallEventAfterStartWorker(
    int active_version_id,
    const StatusCallback& callback) {
  DCHECK_EQ(RUNNING, running_status())
      << "Worker stopped too soon after it was started.";

  int request_id = install_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status = embedded_worker_->SendMessage(
      ServiceWorkerMsg_InstallEvent(request_id, active_version_id));
  if (status != SERVICE_WORKER_OK) {
    install_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::DispatchActivateEventAfterStartWorker(
    const StatusCallback& callback) {
  DCHECK_EQ(RUNNING, running_status())
      << "Worker stopped too soon after it was started.";

  int request_id = activate_callbacks_.Add(new StatusCallback(callback));
  ServiceWorkerStatusCode status =
      embedded_worker_->SendMessage(ServiceWorkerMsg_ActivateEvent(request_id));
  if (status != SERVICE_WORKER_OK) {
    activate_callbacks_.Remove(request_id);
    RunSoon(base::Bind(callback, status));
  }
}

void ServiceWorkerVersion::OnGetClientDocuments(int request_id) {
  if (controllee_by_id_.IsEmpty()) {
    if (running_status() == RUNNING) {
      embedded_worker_->SendMessage(
          ServiceWorkerMsg_DidGetClientDocuments(request_id,
              std::vector<ServiceWorkerClientInfo>()));
    }
    return;
  }
  scoped_refptr<GetClientDocumentsCallback> callback(
      new GetClientDocumentsCallback(request_id, controllee_by_id_.size()));
  ControlleeByIDMap::iterator it(&controllee_by_id_);
  TRACE_EVENT0("ServiceWorker",
               "ServiceWorkerVersion::OnGetClientDocuments");
  while (!it.IsAtEnd()) {
    int client_request_id = get_client_info_callbacks_.Add(
        new GetClientInfoCallback(it.GetCurrentKey(), callback));
    it.GetCurrentValue()->GetClientInfo(embedded_worker_->embedded_worker_id(),
                                        client_request_id);
    it.Advance();
  }
}

void ServiceWorkerVersion::OnGetClientInfoSuccess(
    int request_id,
    const ServiceWorkerClientInfo& info) {
  GetClientInfoCallback* callback =
      get_client_info_callbacks_.Lookup(request_id);
  if (!callback) {
    // The callback may already have been cleared by OnStopped, just ignore.
    return;
  }
  callback->OnSuccess(this, info);
  get_client_info_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnGetClientInfoError(int request_id) {
  GetClientInfoCallback* callback =
      get_client_info_callbacks_.Lookup(request_id);
  if (!callback) {
    // The callback may already have been cleared by OnStopped, just ignore.
    return;
  }
  callback->OnError(this);
  get_client_info_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnActivateEventFinished(
    int request_id,
    blink::WebServiceWorkerEventResult result) {
  DCHECK(ACTIVATING == status() ||
         REDUNDANT == status()) << status();
  TRACE_EVENT0("ServiceWorker",
               "ServiceWorkerVersion::OnActivateEventFinished");

  StatusCallback* callback = activate_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }
  ServiceWorkerStatusCode rv = SERVICE_WORKER_OK;
  if (result == blink::WebServiceWorkerEventResultRejected ||
      status() != ACTIVATING) {
    rv = SERVICE_WORKER_ERROR_ACTIVATE_WORKER_FAILED;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(rv);
  activate_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnInstallEventFinished(
    int request_id,
    blink::WebServiceWorkerEventResult result) {
  DCHECK_EQ(INSTALLING, status()) << status();
  TRACE_EVENT0("ServiceWorker",
               "ServiceWorkerVersion::OnInstallEventFinished");

  StatusCallback* callback = install_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }
  ServiceWorkerStatusCode status = SERVICE_WORKER_OK;
  if (result == blink::WebServiceWorkerEventResultRejected)
    status = SERVICE_WORKER_ERROR_INSTALL_WORKER_FAILED;

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(status);
  install_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnFetchEventFinished(
    int request_id,
    ServiceWorkerFetchEventResult result,
    const ServiceWorkerResponse& response) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnFetchEventFinished",
               "Request id", request_id);
  FetchCallback* callback = fetch_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(SERVICE_WORKER_OK, result, response);
  fetch_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnSyncEventFinished(
    int request_id) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnSyncEventFinished",
               "Request id", request_id);
  StatusCallback* callback = sync_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(SERVICE_WORKER_OK);
  sync_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnNotificationClickEventFinished(
    int request_id) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnNotificationClickEventFinished",
               "Request id", request_id);
  StatusCallback* callback = notification_click_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(SERVICE_WORKER_OK);
  notification_click_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnPushEventFinished(
    int request_id,
    blink::WebServiceWorkerEventResult result) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnPushEventFinished",
               "Request id", request_id);
  StatusCallback* callback = push_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }
  ServiceWorkerStatusCode status = SERVICE_WORKER_OK;
  if (result == blink::WebServiceWorkerEventResultRejected)
    status = SERVICE_WORKER_ERROR_EVENT_WAITUNTIL_REJECTED;

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(status);
  push_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnGeofencingEventFinished(int request_id) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnGeofencingEventFinished",
               "Request id",
               request_id);
  StatusCallback* callback = geofencing_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(SERVICE_WORKER_OK);
  geofencing_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnCrossOriginConnectEventFinished(
    int request_id,
    bool accept_connection) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnCrossOriginConnectEventFinished",
               "Request id", request_id);
  CrossOriginConnectCallback* callback =
      cross_origin_connect_callbacks_.Lookup(request_id);
  if (!callback) {
    NOTREACHED() << "Got unexpected message: " << request_id;
    return;
  }

  scoped_refptr<ServiceWorkerVersion> protect(this);
  callback->Run(SERVICE_WORKER_OK, accept_connection);
  cross_origin_connect_callbacks_.Remove(request_id);
}

void ServiceWorkerVersion::OnPostMessageToDocument(
    int client_id,
    const base::string16& message,
    const std::vector<int>& sent_message_port_ids) {
  TRACE_EVENT1("ServiceWorker",
               "ServiceWorkerVersion::OnPostMessageToDocument",
               "Client id", client_id);
  ServiceWorkerProviderHost* provider_host =
      controllee_by_id_.Lookup(client_id);
  if (!provider_host) {
    // The client may already have been closed, just ignore.
    return;
  }
  provider_host->PostMessage(message, sent_message_port_ids);
}

void ServiceWorkerVersion::OnFocusClient(int request_id, int client_id) {
  TRACE_EVENT2("ServiceWorker",
               "ServiceWorkerVersion::OnFocusDocument",
               "Request id", request_id,
               "Client id", client_id);
  ServiceWorkerProviderHost* provider_host =
      controllee_by_id_.Lookup(client_id);
  if (!provider_host) {
    // The client may already have been closed, just ignore.
    return;
  }

  provider_host->Focus(
      base::Bind(&ServiceWorkerVersion::OnFocusClientFinished,
                 weak_factory_.GetWeakPtr(),
                 request_id));
}

void ServiceWorkerVersion::OnFocusClientFinished(int request_id, bool result) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  if (running_status() != RUNNING)
    return;

  embedded_worker_->SendMessage(ServiceWorkerMsg_FocusClientResponse(
      request_id, result));
}

void ServiceWorkerVersion::OnSkipWaiting(int request_id) {
  skip_waiting_ = true;
  if (status_ != INSTALLED)
    return DidSkipWaiting(request_id);

  if (!context_)
    return;
  ServiceWorkerRegistration* registration =
      context_->GetLiveRegistration(registration_id_);
  if (!registration)
    return;
  pending_skip_waiting_requests_.push_back(request_id);
  if (pending_skip_waiting_requests_.size() == 1)
    registration->ActivateWaitingVersionWhenReady();
}

void ServiceWorkerVersion::DidSkipWaiting(int request_id) {
  if (running_status() == STARTING || running_status() == RUNNING)
    embedded_worker_->SendMessage(ServiceWorkerMsg_DidSkipWaiting(request_id));
}

void ServiceWorkerVersion::ScheduleStopWorker() {
  if (running_status() != RUNNING)
    return;
  if (stop_worker_timer_.IsRunning()) {
    stop_worker_timer_.Reset();
    return;
  }
  stop_worker_timer_.Start(
      FROM_HERE, base::TimeDelta::FromSeconds(kStopWorkerDelay),
      base::Bind(&ServiceWorkerVersion::StopWorkerIfIdle,
                 weak_factory_.GetWeakPtr()));
}

void ServiceWorkerVersion::StopWorkerIfIdle() {
  // Reschedule the stop the worker while there're inflight requests.
  // (Note: we'll probably need to revisit this so that we can kill 'bad' SW.
  // See https://github.com/slightlyoff/ServiceWorker/issues/527)
  if (HasInflightRequests()) {
    ScheduleStopWorker();
    return;
  }
  if (running_status() == STOPPED || !stop_callbacks_.empty())
    return;
  embedded_worker_->StopIfIdle();
}

bool ServiceWorkerVersion::HasInflightRequests() const {
  return
    !activate_callbacks_.IsEmpty() ||
    !install_callbacks_.IsEmpty() ||
    !fetch_callbacks_.IsEmpty() ||
    !sync_callbacks_.IsEmpty() ||
    !notification_click_callbacks_.IsEmpty() ||
    !push_callbacks_.IsEmpty() ||
    !geofencing_callbacks_.IsEmpty();
}

void ServiceWorkerVersion::DoomInternal() {
  DCHECK(!HasControllee());
  SetStatus(REDUNDANT);
  StopWorker(base::Bind(&ServiceWorkerUtils::NoOpStatusCallback));
  if (!context_)
    return;
  std::vector<ServiceWorkerDatabase::ResourceRecord> resources;
  script_cache_map_.GetResources(&resources);
  context_->storage()->PurgeResources(resources);
}

}  // namespace content
