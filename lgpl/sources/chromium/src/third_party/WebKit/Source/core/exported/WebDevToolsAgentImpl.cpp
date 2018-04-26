/*
 * Copyright (C) 2010-2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/exported/WebDevToolsAgentImpl.h"

#include <v8-inspector.h>
#include <memory>
#include <utility>

#include "bindings/core/v8/ScriptController.h"
#include "bindings/core/v8/V8BindingForCore.h"
#include "core/CoreInitializer.h"
#include "core/CoreProbeSink.h"
#include "core/events/WebInputEventConversion.h"
#include "core/exported/WebSettingsImpl.h"
#include "core/exported/WebViewImpl.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/LocalFrameView.h"
#include "core/frame/Settings.h"
#include "core/frame/WebLocalFrameImpl.h"
#include "core/inspector/DevToolsEmulator.h"
#include "core/inspector/InspectedFrames.h"
#include "core/inspector/InspectorAnimationAgent.h"
#include "core/inspector/InspectorApplicationCacheAgent.h"
#include "core/inspector/InspectorAuditsAgent.h"
#include "core/inspector/InspectorCSSAgent.h"
#include "core/inspector/InspectorDOMAgent.h"
#include "core/inspector/InspectorDOMDebuggerAgent.h"
#include "core/inspector/InspectorDOMSnapshotAgent.h"
#include "core/inspector/InspectorEmulationAgent.h"
#include "core/inspector/InspectorIOAgent.h"
#include "core/inspector/InspectorLayerTreeAgent.h"
#include "core/inspector/InspectorLogAgent.h"
#include "core/inspector/InspectorMemoryAgent.h"
#include "core/inspector/InspectorNetworkAgent.h"
#include "core/inspector/InspectorOverlayAgent.h"
#include "core/inspector/InspectorPageAgent.h"
#include "core/inspector/InspectorPerformanceAgent.h"
#include "core/inspector/InspectorResourceContainer.h"
#include "core/inspector/InspectorResourceContentLoader.h"
#include "core/inspector/InspectorSession.h"
#include "core/inspector/InspectorTaskRunner.h"
#include "core/inspector/InspectorTracingAgent.h"
#include "core/inspector/InspectorWorkerAgent.h"
#include "core/inspector/MainThreadDebugger.h"
#include "core/layout/LayoutView.h"
#include "core/page/FocusController.h"
#include "core/page/Page.h"
#include "core/probe/CoreProbes.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "platform/CrossThreadFunctional.h"
#include "platform/LayoutTestSupport.h"
#include "platform/WebTaskRunner.h"
#include "platform/graphics/GraphicsContext.h"
#include "platform/graphics/paint/PaintController.h"
#include "platform/instrumentation/tracing/TraceEvent.h"
#include "platform/wtf/MathExtras.h"
#include "platform/wtf/PtrUtil.h"
#include "platform/wtf/Vector.h"
#include "platform/wtf/text/WTFString.h"
#include "public/platform/Platform.h"
#include "public/platform/WebFloatRect.h"
#include "public/platform/WebLayerTreeView.h"
#include "public/platform/WebRect.h"
#include "public/platform/WebString.h"
#include "public/web/WebSettings.h"
#include "public/web/WebViewClient.h"

namespace blink {

namespace {

bool IsMainFrame(WebLocalFrameImpl* frame) {
  // TODO(dgozman): sometimes view->mainFrameImpl() does return null, even
  // though |frame| is meant to be main frame.  See http://crbug.com/526162.
  return frame->ViewImpl() && !frame->Parent();
}

// TODO(dgozman): somehow get this from a mojo config.
// See kMaximumMojoMessageSize in services/service_manager/embedder/main.cc.
const size_t kMaxDevToolsMessageChunkSize = 128 * 1024 * 1024 / 8;

bool ShouldInterruptForMethod(const String& method) {
  // Keep in sync with DevToolsSession::ShouldSendOnIO.
  // TODO(dgozman): find a way to share this.
  return method == "Debugger.pause" || method == "Debugger.setBreakpoint" ||
         method == "Debugger.setBreakpointByUrl" ||
         method == "Debugger.removeBreakpoint" ||
         method == "Debugger.setBreakpointsActive" ||
         method == "Performance.getMetrics" || method == "Page.crash";
}

}  // namespace

class ClientMessageLoopAdapter : public MainThreadDebugger::ClientMessageLoop {
 public:
  ~ClientMessageLoopAdapter() override { instance_ = nullptr; }

  static void EnsureMainThreadDebuggerCreated() {
    if (instance_)
      return;
    std::unique_ptr<ClientMessageLoopAdapter> instance(
        new ClientMessageLoopAdapter(
            Platform::Current()->CreateNestedMessageLoopRunner()));
    instance_ = instance.get();
    MainThreadDebugger::Instance()->SetClientMessageLoop(std::move(instance));
  }

  static void ContinueProgram() {
    // Release render thread if necessary.
    if (instance_)
      instance_->QuitNow();
  }

 private:
  ClientMessageLoopAdapter(
      std::unique_ptr<Platform::NestedMessageLoopRunner> message_loop)
      : running_for_debug_break_(false),
        message_loop_(std::move(message_loop)) {
    DCHECK(message_loop_.get());
  }

  void Run(LocalFrame* frame) override {
    if (running_for_debug_break_)
      return;

    running_for_debug_break_ = true;
    RunLoop(WebLocalFrameImpl::FromFrame(frame));
  }

  void RunLoop(WebLocalFrameImpl* frame) {
    // 0. Flush pending frontend messages.
    WebDevToolsAgentImpl* agent = frame->DevToolsAgentImpl();
    agent->FlushProtocolNotifications();

    // 1. Disable input events.
    WebFrameWidgetBase::SetIgnoreInputEvents(true);
    for (const auto view : WebViewImpl::AllInstances())
      view->GetChromeClient().NotifyPopupOpeningObservers();

    // 2. Disable active objects
    WebView::WillEnterModalLoop();

    // 3. Process messages until quitNow is called.
    message_loop_->Run();

    // 4. Resume active objects
    WebView::DidExitModalLoop();

    // 5. Enable input events.
    WebFrameWidgetBase::SetIgnoreInputEvents(false);
  }

  void QuitNow() override {
    if (running_for_debug_break_) {
      running_for_debug_break_ = false;
      message_loop_->QuitNow();
    }
  }

  void RunIfWaitingForDebugger(LocalFrame* frame) override {
    WebDevToolsAgentImpl* agent =
        WebLocalFrameImpl::FromFrame(frame)->DevToolsAgentImpl();
    if (agent && agent->worker_client_)
      agent->worker_client_->ResumeStartup();
  }

  bool running_for_debug_break_;
  std::unique_ptr<Platform::NestedMessageLoopRunner> message_loop_;

  static ClientMessageLoopAdapter* instance_;
};

ClientMessageLoopAdapter* ClientMessageLoopAdapter::instance_ = nullptr;

// --------- WebDevToolsAgentImpl::Session -------------

class WebDevToolsAgentImpl::Session : public GarbageCollectedFinalized<Session>,
                                      public mojom::blink::DevToolsSession,
                                      public InspectorSession::Client {
 public:
  Session(WebDevToolsAgentImpl*,
          mojom::blink::DevToolsSessionHostAssociatedPtrInfo host_ptr_info,
          mojom::blink::DevToolsSessionAssociatedRequest main_request,
          mojom::blink::DevToolsSessionRequest io_request,
          const String& reattach_state);
  ~Session() override;

  virtual void Trace(blink::Visitor*);
  void Detach();

  InspectorSession* inspector_session() { return inspector_session_.Get(); }
  InspectorPageAgent* page_agent() { return page_agent_.Get(); }
  InspectorTracingAgent* tracing_agent() { return tracing_agent_.Get(); }
  InspectorOverlayAgent* overlay_agent() { return overlay_agent_.Get(); }

 private:
  class IOSession;

  // mojom::blink::DevToolsSession implementation.
  void DispatchProtocolMessage(int call_id,
                               const String& method,
                               const String& message) override;
  void InspectElement(const WebPoint& point_in_root_frame) override;

  // InspectorSession::Client implementation.
  void SendProtocolMessage(int session_id,
                           int call_id,
                           const String& response,
                           const String& state) override;

  void DispatchProtocolMessageInternal(int call_id,
                                       const String& method,
                                       const String& message);
  void InitializeInspectorSession(const String& reattach_state);

  Member<WebDevToolsAgentImpl> agent_;
  Member<WebLocalFrameImpl> frame_;
  mojo::AssociatedBinding<mojom::blink::DevToolsSession> binding_;
  mojom::blink::DevToolsSessionHostAssociatedPtr host_ptr_;
  IOSession* io_session_;
  Member<InspectorSession> inspector_session_;
  Member<InspectorPageAgent> page_agent_;
  Member<InspectorTracingAgent> tracing_agent_;
  Member<InspectorOverlayAgent> overlay_agent_;
  bool detached_ = false;

  DISALLOW_COPY_AND_ASSIGN(Session);
};

// Created and stored in unique_ptr on UI.
// Binds request, receives messages and destroys on IO.
class WebDevToolsAgentImpl::Session::IOSession
    : public mojom::blink::DevToolsSession {
 public:
  IOSession(scoped_refptr<base::SingleThreadTaskRunner> session_task_runner,
            scoped_refptr<WebTaskRunner> agent_task_runner,
            CrossThreadWeakPersistent<WebDevToolsAgentImpl::Session> session,
            mojom::blink::DevToolsSessionRequest request)
      : session_task_runner_(session_task_runner),
        agent_task_runner_(agent_task_runner),
        session_(std::move(session)),
        binding_(this) {
    session_task_runner->PostTask(
        FROM_HERE, ConvertToBaseCallback(CrossThreadBind(
                       &IOSession::BindInterface, CrossThreadUnretained(this),
                       WTF::Passed(std::move(request)))));
  }

  ~IOSession() override {}

  void BindInterface(mojom::blink::DevToolsSessionRequest request) {
    binding_.Bind(std::move(request));
  }

  void DeleteSoon() { session_task_runner_->DeleteSoon(FROM_HERE, this); }

  // mojom::blink::DevToolsSession implementation.
  void DispatchProtocolMessage(int call_id,
                               const String& method,
                               const String& message) override {
    DCHECK(ShouldInterruptForMethod(method));
    // Crash renderer.
    if (method == "Page.crash")
      CHECK(false);

    MainThreadDebugger::InterruptMainThreadAndRun(CrossThreadBind(
        &WebDevToolsAgentImpl::Session::DispatchProtocolMessageInternal,
        session_, call_id, method, message));
    PostCrossThreadTask(
        *agent_task_runner_, FROM_HERE,
        CrossThreadBind(&WebDevToolsAgentImpl::Session::DispatchProtocolMessage,
                        session_, call_id, method, message));
  }

  void InspectElement(const WebPoint&) override { NOTREACHED(); }

 private:
  scoped_refptr<base::SingleThreadTaskRunner> session_task_runner_;
  scoped_refptr<WebTaskRunner> agent_task_runner_;
  CrossThreadWeakPersistent<WebDevToolsAgentImpl::Session> session_;
  mojo::Binding<mojom::blink::DevToolsSession> binding_;

  DISALLOW_COPY_AND_ASSIGN(IOSession);
};

WebDevToolsAgentImpl::Session::Session(
    WebDevToolsAgentImpl* agent,
    mojom::blink::DevToolsSessionHostAssociatedPtrInfo host_ptr_info,
    mojom::blink::DevToolsSessionAssociatedRequest request,
    mojom::blink::DevToolsSessionRequest io_request,
    const String& reattach_state)
    : agent_(agent),
      frame_(agent->web_local_frame_impl_),
      binding_(this, std::move(request)) {
  io_session_ =
      new IOSession(Platform::Current()->GetIOTaskRunner(),
                    frame_->GetFrame()->GetTaskRunner(TaskType::kUnthrottled),
                    WrapCrossThreadWeakPersistent(this), std::move(io_request));

  host_ptr_.Bind(std::move(host_ptr_info));
  host_ptr_.set_connection_error_handler(WTF::Bind(
      &WebDevToolsAgentImpl::Session::Detach, WrapWeakPersistent(this)));

  InitializeInspectorSession(reattach_state);
}

WebDevToolsAgentImpl::Session::~Session() {
  DCHECK(detached_);
}

void WebDevToolsAgentImpl::Session::Trace(blink::Visitor* visitor) {
  visitor->Trace(agent_);
  visitor->Trace(frame_);
  visitor->Trace(inspector_session_);
  visitor->Trace(page_agent_);
  visitor->Trace(tracing_agent_);
  visitor->Trace(overlay_agent_);
}

void WebDevToolsAgentImpl::Session::Detach() {
  DCHECK(!detached_);
  detached_ = true;
  agent_->DetachSession(this);
  binding_.Close();
  host_ptr_.reset();
  io_session_->DeleteSoon();
  io_session_ = nullptr;
  inspector_session_->Dispose();
}

void WebDevToolsAgentImpl::Session::DispatchProtocolMessage(
    int call_id,
    const String& method,
    const String& message) {
  if (ShouldInterruptForMethod(method))
    MainThreadDebugger::Instance()->TaskRunner()->RunAllTasksDontWait();
  else
    DispatchProtocolMessageInternal(call_id, method, message);
}

void WebDevToolsAgentImpl::Session::InspectElement(
    const WebPoint& point_in_root_frame) {
  WebPoint point = point_in_root_frame;
  if (frame_->ViewImpl() && frame_->ViewImpl()->Client()) {
    WebFloatRect rect(point.x, point.y, 0, 0);
    frame_->ViewImpl()->Client()->ConvertWindowToViewport(&rect);
    point = WebPoint(rect.x, rect.y);
  }

  HitTestRequest::HitTestRequestType hit_type =
      HitTestRequest::kMove | HitTestRequest::kReadOnly |
      HitTestRequest::kAllowChildFrameContent;
  HitTestRequest request(hit_type);
  WebMouseEvent dummy_event(WebInputEvent::kMouseDown,
                            WebInputEvent::kNoModifiers,
                            WTF::CurrentTimeTicksInMilliseconds());
  dummy_event.SetPositionInWidget(point.x, point.y);
  IntPoint transformed_point = FlooredIntPoint(
      TransformWebMouseEvent(frame_->GetFrameView(), dummy_event)
          .PositionInRootFrame());
  HitTestResult result(
      request, frame_->GetFrameView()->RootFrameToContents(transformed_point));
  frame_->GetFrame()->ContentLayoutObject()->HitTest(result);
  Node* node = result.InnerNode();
  if (!node && frame_->GetFrame()->GetDocument())
    node = frame_->GetFrame()->GetDocument()->documentElement();
  overlay_agent_->Inspect(node);
}

void WebDevToolsAgentImpl::Session::SendProtocolMessage(int session_id,
                                                        int call_id,
                                                        const String& response,
                                                        const String& state) {
  if (detached_)
    return;

  // Make tests more predictable by flushing all sessions before sending
  // protocol response in any of them.
  if (LayoutTestSupport::IsRunningLayoutTest() && call_id)
    agent_->FlushProtocolNotifications();

  bool single_chunk = response.length() < kMaxDevToolsMessageChunkSize;
  for (size_t pos = 0; pos < response.length();
       pos += kMaxDevToolsMessageChunkSize) {
    mojom::blink::DevToolsMessageChunkPtr chunk =
        mojom::blink::DevToolsMessageChunk::New();
    chunk->is_first = pos == 0;
    chunk->is_last = pos + kMaxDevToolsMessageChunkSize >= response.length();
    chunk->call_id = chunk->is_last ? call_id : 0;
    chunk->post_state =
        chunk->is_last && !state.IsNull() ? state : g_empty_string;
    chunk->data = single_chunk
                      ? response
                      : response.Substring(pos, kMaxDevToolsMessageChunkSize);
    host_ptr_->DispatchProtocolMessage(std::move(chunk));
  }
}

void WebDevToolsAgentImpl::Session::DispatchProtocolMessageInternal(
    int call_id,
    const String& method,
    const String& message) {
  // IOSession does not provide ordering guarantees relative to
  // Session, so a command may come to IOSession after Session is detached,
  // and get posted to main thread to this method.
  //
  // At the same time, Session may not be garbage collected yet
  // (even though already detached), and CrossThreadWeakPersistent<Session>
  // will still be valid.
  //
  // Both these factors combined may lead to this method being called after
  // detach, so we have to check a flag here.
  if (detached_)
    return;
  InspectorTaskRunner::IgnoreInterruptsScope scope(
      MainThreadDebugger::Instance()->TaskRunner());
  inspector_session_->DispatchProtocolMessage(method, message);
}

void WebDevToolsAgentImpl::Session::InitializeInspectorSession(
    const String& reattach_state) {
  String state = reattach_state;
  // TODO(dgozman): make InspectorSession check for IsNull() instead.
  String* state_ptr = nullptr;
  if (!reattach_state.IsNull())
    state_ptr = &state;

  ClientMessageLoopAdapter::EnsureMainThreadDebuggerCreated();
  MainThreadDebugger* main_thread_debugger = MainThreadDebugger::Instance();
  v8::Isolate* isolate = V8PerIsolateData::MainThreadIsolate();
  InspectedFrames* inspected_frames = agent_->inspected_frames_.Get();

  inspector_session_ = new InspectorSession(
      this, agent_->probe_sink_.Get(), 0,
      main_thread_debugger->GetV8Inspector(),
      main_thread_debugger->ContextGroupId(inspected_frames->Root()),
      state_ptr);

  InspectorDOMAgent* dom_agent = new InspectorDOMAgent(
      isolate, inspected_frames, inspector_session_->V8Session());
  inspector_session_->Append(dom_agent);

  InspectorLayerTreeAgent* layer_tree_agent =
      InspectorLayerTreeAgent::Create(inspected_frames, agent_);
  inspector_session_->Append(layer_tree_agent);

  InspectorNetworkAgent* network_agent = new InspectorNetworkAgent(
      inspected_frames, nullptr, inspector_session_->V8Session());
  inspector_session_->Append(network_agent);

  InspectorCSSAgent* css_agent =
      InspectorCSSAgent::Create(dom_agent, inspected_frames, network_agent,
                                agent_->resource_content_loader_.Get(),
                                agent_->resource_container_.Get());
  inspector_session_->Append(css_agent);

  InspectorDOMDebuggerAgent* dom_debugger_agent = new InspectorDOMDebuggerAgent(
      isolate, dom_agent, inspector_session_->V8Session());
  inspector_session_->Append(dom_debugger_agent);

  inspector_session_->Append(
      InspectorDOMSnapshotAgent::Create(inspected_frames, dom_debugger_agent));

  inspector_session_->Append(new InspectorAnimationAgent(
      inspected_frames, css_agent, inspector_session_->V8Session()));

  inspector_session_->Append(InspectorMemoryAgent::Create(inspected_frames));

  inspector_session_->Append(
      InspectorPerformanceAgent::Create(inspected_frames));

  inspector_session_->Append(
      InspectorApplicationCacheAgent::Create(inspected_frames));

  InspectorWorkerAgent* worker_agent =
      new InspectorWorkerAgent(inspected_frames);
  inspector_session_->Append(worker_agent);

  tracing_agent_ =
      InspectorTracingAgent::Create(agent_, worker_agent, inspected_frames);
  inspector_session_->Append(tracing_agent_);

  page_agent_ = InspectorPageAgent::Create(
      inspected_frames, agent_, agent_->resource_content_loader_.Get(),
      inspector_session_->V8Session());
  inspector_session_->Append(page_agent_);

  inspector_session_->Append(new InspectorLogAgent(
      &inspected_frames->Root()->GetPage()->GetConsoleMessageStorage(),
      inspected_frames->Root()->GetPerformanceMonitor(),
      inspector_session_->V8Session()));

  overlay_agent_ =
      new InspectorOverlayAgent(frame_.Get(), inspected_frames,
                                inspector_session_->V8Session(), dom_agent);
  inspector_session_->Append(overlay_agent_);

  inspector_session_->Append(
      new InspectorIOAgent(isolate, inspector_session_->V8Session()));

  inspector_session_->Append(new InspectorAuditsAgent(network_agent));

  tracing_agent_->SetLayerTreeId(agent_->layer_tree_id_);

  if (agent_->include_view_agents_) {
    // TODO(dgozman): we should actually pass the view instead of frame, but
    // during remote->local transition we cannot access mainFrameImpl() yet, so
    // we have to store the frame which will become the main frame later.
    inspector_session_->Append(new InspectorEmulationAgent(frame_.Get()));
  }

  // Call session init callbacks registered from higher layers
  CoreInitializer::GetInstance().InitInspectorAgentSession(
      inspector_session_, agent_->include_view_agents_, dom_agent,
      inspected_frames, frame_->ViewImpl()->GetPage());

  if (!reattach_state.IsNull())
    inspector_session_->Restore();

  // TODO(dgozman): do not send empty state from the browser side.
  if (agent_->worker_client_ && !reattach_state.IsEmpty())
    agent_->worker_client_->ResumeStartup();
}

// --------- WebDevToolsAgentImpl -------------

// static
WebDevToolsAgentImpl* WebDevToolsAgentImpl::CreateForFrame(
    WebLocalFrameImpl* frame) {
  if (!IsMainFrame(frame)) {
    WebDevToolsAgentImpl* agent =
        new WebDevToolsAgentImpl(frame, false, nullptr);
    if (frame->FrameWidget())
      agent->LayerTreeViewChanged(frame->FrameWidget()->GetLayerTreeView());
    return agent;
  }

  WebViewImpl* view = frame->ViewImpl();
  WebDevToolsAgentImpl* agent = new WebDevToolsAgentImpl(frame, true, nullptr);
  agent->LayerTreeViewChanged(view->LayerTreeView());
  return agent;
}

// static
WebDevToolsAgentImpl* WebDevToolsAgentImpl::CreateForWorker(
    WebLocalFrameImpl* frame,
    WorkerClient* worker_client) {
  WebViewImpl* view = frame->ViewImpl();
  WebDevToolsAgentImpl* agent =
      new WebDevToolsAgentImpl(frame, true, worker_client);
  agent->LayerTreeViewChanged(view->LayerTreeView());
  return agent;
}

WebDevToolsAgentImpl::WebDevToolsAgentImpl(
    WebLocalFrameImpl* web_local_frame_impl,
    bool include_view_agents,
    WorkerClient* worker_client)
    : binding_(this),
      worker_client_(worker_client),
      web_local_frame_impl_(web_local_frame_impl),
      probe_sink_(web_local_frame_impl_->GetFrame()->GetProbeSink()),
      resource_content_loader_(InspectorResourceContentLoader::Create(
          web_local_frame_impl_->GetFrame())),
      inspected_frames_(new InspectedFrames(
          web_local_frame_impl_->GetFrame(),
          web_local_frame_impl_->GetFrame()->GetDevToolsFrameToken())),
      resource_container_(new InspectorResourceContainer(inspected_frames_)),
      include_view_agents_(include_view_agents),
      layer_tree_id_(0) {
  DCHECK(IsMainThread());
  DCHECK(web_local_frame_impl_->GetFrame());
}

WebDevToolsAgentImpl::~WebDevToolsAgentImpl() {
  DCHECK(!worker_client_);
}

void WebDevToolsAgentImpl::Trace(blink::Visitor* visitor) {
  visitor->Trace(sessions_);
  visitor->Trace(web_local_frame_impl_);
  visitor->Trace(probe_sink_);
  visitor->Trace(resource_content_loader_);
  visitor->Trace(inspected_frames_);
  visitor->Trace(resource_container_);
}

void WebDevToolsAgentImpl::WillBeDestroyed() {
  DCHECK(web_local_frame_impl_->GetFrame());
  DCHECK(inspected_frames_->Root()->View());

  HeapHashSet<Member<Session>> copy(sessions_);
  for (auto& session : copy)
    session->Detach();

  resource_content_loader_->Dispose();
  worker_client_ = nullptr;
  binding_.Close();
}

void WebDevToolsAgentImpl::BindRequest(
    mojom::blink::DevToolsAgentAssociatedRequest request) {
  binding_.Bind(std::move(request));
}

void WebDevToolsAgentImpl::AttachDevToolsSession(
    mojom::blink::DevToolsSessionHostAssociatedPtrInfo host,
    mojom::blink::DevToolsSessionAssociatedRequest session,
    mojom::blink::DevToolsSessionRequest io_session,
    const String& reattach_state) {
  if (!sessions_.size())
    Platform::Current()->CurrentThread()->AddTaskObserver(this);
  sessions_.insert(new Session(this, std::move(host), std::move(session),
                               std::move(io_session), reattach_state));
}

void WebDevToolsAgentImpl::DetachSession(Session* session) {
  sessions_.erase(session);
  if (!sessions_.size())
    Platform::Current()->CurrentThread()->RemoveTaskObserver(this);
}

void WebDevToolsAgentImpl::DidCommitLoadForLocalFrame(LocalFrame* frame) {
  resource_container_->DidCommitLoadForLocalFrame(frame);
  resource_content_loader_->DidCommitLoadForLocalFrame(frame);
  for (auto& session : sessions_)
    session->inspector_session()->DidCommitLoadForLocalFrame(frame);
}

void WebDevToolsAgentImpl::DidStartProvisionalLoad(LocalFrame* frame) {
  if (inspected_frames_->Root() == frame) {
    for (auto& session : sessions_)
      session->inspector_session()->V8Session()->resume();
  }
}

bool WebDevToolsAgentImpl::ScreencastEnabled() {
  for (auto& session : sessions_) {
    if (session->page_agent()->ScreencastEnabled())
      return true;
  }
  return false;
}

void WebDevToolsAgentImpl::RootLayerCleared() {
  for (auto& session : sessions_)
    session->tracing_agent()->RootLayerCleared();
}

void WebDevToolsAgentImpl::LayerTreeViewChanged(
    WebLayerTreeView* layer_tree_view) {
  layer_tree_id_ = layer_tree_view ? layer_tree_view->LayerTreeId() : 0;
  for (auto& session : sessions_)
    session->tracing_agent()->SetLayerTreeId(layer_tree_id_);
}

void WebDevToolsAgentImpl::ShowReloadingBlanket() {
  for (auto& session : sessions_)
    session->overlay_agent()->ShowReloadingBlanket();
}

void WebDevToolsAgentImpl::HideReloadingBlanket() {
  for (auto& session : sessions_)
    session->overlay_agent()->HideReloadingBlanket();
}

void WebDevToolsAgentImpl::PageLayoutInvalidated(bool resized) {
  for (auto& session : sessions_)
    session->overlay_agent()->PageLayoutInvalidated(resized);
}

bool WebDevToolsAgentImpl::IsInspectorLayer(GraphicsLayer* layer) {
  for (auto& session : sessions_) {
    if (session->overlay_agent()->IsInspectorLayer(layer))
      return true;
  }
  return false;
}

String WebDevToolsAgentImpl::EvaluateInOverlayForTesting(const String& script) {
  String result;
  for (auto& session : sessions_)
    result = session->overlay_agent()->EvaluateInOverlayForTest(script);
  return result;
}

void WebDevToolsAgentImpl::PaintOverlay() {
  for (auto& session : sessions_)
    session->overlay_agent()->PaintOverlay();
}

void WebDevToolsAgentImpl::LayoutOverlay() {
  for (auto& session : sessions_)
    session->overlay_agent()->LayoutOverlay();
}

void WebDevToolsAgentImpl::DispatchBufferedTouchEvents() {
  for (auto& session : sessions_)
    session->overlay_agent()->DispatchBufferedTouchEvents();
}

bool WebDevToolsAgentImpl::HandleInputEvent(const WebInputEvent& event) {
  for (auto& session : sessions_) {
    if (session->overlay_agent()->HandleInputEvent(event))
      return true;
  }
  return false;
}

void WebDevToolsAgentImpl::FlushProtocolNotifications() {
  for (auto& session : sessions_)
    session->inspector_session()->flushProtocolNotifications();
}

void WebDevToolsAgentImpl::WillProcessTask() {
  if (sessions_.IsEmpty())
    return;
  ThreadDebugger::IdleFinished(V8PerIsolateData::MainThreadIsolate());
}

void WebDevToolsAgentImpl::DidProcessTask() {
  if (sessions_.IsEmpty())
    return;
  ThreadDebugger::IdleStarted(V8PerIsolateData::MainThreadIsolate());
  FlushProtocolNotifications();
}

}  // namespace blink
