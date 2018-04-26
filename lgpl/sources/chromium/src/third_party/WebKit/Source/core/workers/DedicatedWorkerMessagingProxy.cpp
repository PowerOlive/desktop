// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/workers/DedicatedWorkerMessagingProxy.h"

#include <memory>
#include "bindings/core/v8/V8CacheOptions.h"
#include "core/dom/Document.h"
#include "core/events/ErrorEvent.h"
#include "core/events/MessageEvent.h"
#include "core/fetch/Request.h"
#include "core/frame/csp/ContentSecurityPolicy.h"
#include "core/inspector/MainThreadDebugger.h"
#include "core/workers/DedicatedWorker.h"
#include "core/workers/DedicatedWorkerObjectProxy.h"
#include "core/workers/DedicatedWorkerThread.h"
#include "core/workers/WorkerInspectorProxy.h"
#include "core/workers/WorkerOptions.h"
#include "platform/CrossThreadFunctional.h"
#include "platform/WebTaskRunner.h"
#include "platform/wtf/WTF.h"
#include "public/platform/TaskType.h"
#include "services/network/public/interfaces/fetch_api.mojom-shared.h"

namespace blink {

struct DedicatedWorkerMessagingProxy::QueuedTask {
  scoped_refptr<SerializedScriptValue> message;
  Vector<MessagePortChannel> channels;
  v8_inspector::V8StackTraceId stack_id;
};

DedicatedWorkerMessagingProxy::DedicatedWorkerMessagingProxy(
    ExecutionContext* execution_context,
    DedicatedWorker* worker_object)
    : ThreadedMessagingProxyBase(execution_context),
      worker_object_(worker_object) {
  worker_object_proxy_ =
      DedicatedWorkerObjectProxy::Create(this, GetParentFrameTaskRunners());
}

DedicatedWorkerMessagingProxy::~DedicatedWorkerMessagingProxy() = default;

void DedicatedWorkerMessagingProxy::StartWorkerGlobalScope(
    std::unique_ptr<GlobalScopeCreationParams> creation_params,
    const WorkerOptions& options,
    const KURL& script_url,
    const v8_inspector::V8StackTraceId& stack_id,
    const String& source_code) {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate()) {
    // Worker.terminate() could be called from JS before the thread was
    // created.
    return;
  }

  InitializeWorkerThread(
      std::move(creation_params),
      CreateBackingThreadStartupData(ToIsolate(GetExecutionContext())));

  if (options.type() == "classic") {
    GetWorkerThread()->EvaluateClassicScript(
        script_url, source_code, nullptr /* cached_meta_data */, stack_id);
  } else if (options.type() == "module") {
    network::mojom::FetchCredentialsMode credentials_mode;
    bool result =
        Request::ParseCredentialsMode(options.credentials(), &credentials_mode);
    DCHECK(result);
    GetWorkerThread()->ImportModuleScript(script_url, credentials_mode);
  } else {
    NOTREACHED();
  }

  // Post all queued tasks to the worker.
  for (auto& queued_task : queued_early_tasks_) {
    WTF::CrossThreadClosure task = CrossThreadBind(
        &DedicatedWorkerObjectProxy::ProcessMessageFromWorkerObject,
        CrossThreadUnretained(&WorkerObjectProxy()),
        WTF::Passed(std::move(queued_task.message)),
        WTF::Passed(std::move(queued_task.channels)),
        CrossThreadUnretained(GetWorkerThread()), queued_task.stack_id);
    PostCrossThreadTask(
        *GetWorkerThread()->GetTaskRunner(TaskType::kPostedMessage), FROM_HERE,
        std::move(task));
  }
  queued_early_tasks_.clear();
}

void DedicatedWorkerMessagingProxy::PostMessageToWorkerGlobalScope(
    scoped_refptr<SerializedScriptValue> message,
    Vector<MessagePortChannel> channels,
    const v8_inspector::V8StackTraceId& stack_id) {
  DCHECK(IsParentContextThread());
  if (AskedToTerminate())
    return;

  if (GetWorkerThread()) {
    WTF::CrossThreadClosure task = CrossThreadBind(
        &DedicatedWorkerObjectProxy::ProcessMessageFromWorkerObject,
        CrossThreadUnretained(&WorkerObjectProxy()), std::move(message),
        WTF::Passed(std::move(channels)),
        CrossThreadUnretained(GetWorkerThread()), stack_id);
    PostCrossThreadTask(
        *GetWorkerThread()->GetTaskRunner(TaskType::kPostedMessage), FROM_HERE,
        std::move(task));
  } else {
    // GetWorkerThread() returns nullptr while the worker thread is being
    // created. In that case, push events into the queue and dispatch them in
    // WorkerThreadCreated().
    queued_early_tasks_.push_back(
        QueuedTask{std::move(message), std::move(channels), stack_id});
  }
}

bool DedicatedWorkerMessagingProxy::HasPendingActivity() const {
  DCHECK(IsParentContextThread());
  return !AskedToTerminate();
}

void DedicatedWorkerMessagingProxy::PostMessageToWorkerObject(
    scoped_refptr<SerializedScriptValue> message,
    Vector<MessagePortChannel> channels,
    const v8_inspector::V8StackTraceId& stack_id) {
  DCHECK(IsParentContextThread());
  if (!worker_object_ || AskedToTerminate())
    return;

  MessagePortArray* ports =
      MessagePort::EntanglePorts(*GetExecutionContext(), std::move(channels));
  MainThreadDebugger::Instance()->ExternalAsyncTaskStarted(stack_id);
  worker_object_->DispatchEvent(
      MessageEvent::Create(ports, std::move(message)));
  MainThreadDebugger::Instance()->ExternalAsyncTaskFinished(stack_id);
}

void DedicatedWorkerMessagingProxy::DispatchErrorEvent(
    const String& error_message,
    std::unique_ptr<SourceLocation> location,
    int exception_id) {
  DCHECK(IsParentContextThread());
  if (!worker_object_)
    return;

  // We don't bother checking the AskedToTerminate() flag for dispatching the
  // event on the owner context, because exceptions should *always* be reported
  // even if the thread is terminated as the spec says:
  //
  // "Thus, error reports propagate up to the chain of dedicated workers up to
  // the original Document, even if some of the workers along this chain have
  // been terminated and garbage collected."
  // https://html.spec.whatwg.org/multipage/workers.html#runtime-script-errors-2
  ErrorEvent* event =
      ErrorEvent::Create(error_message, location->Clone(), nullptr);
  if (worker_object_->DispatchEvent(event) != DispatchEventResult::kNotCanceled)
    return;

  // The worker thread can already be terminated.
  if (!GetWorkerThread()) {
    DCHECK(AskedToTerminate());
    return;
  }

  // The HTML spec requires to queue an error event using the DOM manipulation
  // task source.
  // https://html.spec.whatwg.org/multipage/workers.html#runtime-script-errors-2
  PostCrossThreadTask(
      *GetWorkerThread()->GetTaskRunner(TaskType::kDOMManipulation), FROM_HERE,
      CrossThreadBind(&DedicatedWorkerObjectProxy::ProcessUnhandledException,
                      CrossThreadUnretained(worker_object_proxy_.get()),
                      exception_id, CrossThreadUnretained(GetWorkerThread())));
}

void DedicatedWorkerMessagingProxy::Trace(blink::Visitor* visitor) {
  visitor->Trace(worker_object_);
  ThreadedMessagingProxyBase::Trace(visitor);
}

WTF::Optional<WorkerBackingThreadStartupData>
DedicatedWorkerMessagingProxy::CreateBackingThreadStartupData(
    v8::Isolate* isolate) {
  using HeapLimitMode = WorkerBackingThreadStartupData::HeapLimitMode;
  using AtomicsWaitMode = WorkerBackingThreadStartupData::AtomicsWaitMode;
  return WorkerBackingThreadStartupData(
      isolate->IsHeapLimitIncreasedForDebugging()
          ? HeapLimitMode::kIncreasedForDebugging
          : HeapLimitMode::kDefault,
      AtomicsWaitMode::kAllow);
}

std::unique_ptr<WorkerThread>
DedicatedWorkerMessagingProxy::CreateWorkerThread() {
  return DedicatedWorkerThread::Create(CreateThreadableLoadingContext(),
                                       WorkerObjectProxy());
}

}  // namespace blink
