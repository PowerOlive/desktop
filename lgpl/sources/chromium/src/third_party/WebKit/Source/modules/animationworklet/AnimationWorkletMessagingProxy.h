// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AnimationWorkletMessagingProxy_h
#define AnimationWorkletMessagingProxy_h

#include <memory>
#include "core/dom/AnimationWorkletProxyClient.h"
#include "core/workers/ThreadedWorkletMessagingProxy.h"

namespace blink {

class ExecutionContext;
class WorkerThread;

// Acts as a proxy for the animation worklet global scopes that live on the
// worklet thread. The logic to actually proxy an off thread global scope is
// implemented in its parent. The main contribution of this class is to
// create an appropriate worklet thread type (in this case an
// |AnimationWorkletThread|) as part of the the worklet initialization process.
class AnimationWorkletMessagingProxy final
    : public ThreadedWorkletMessagingProxy {
 public:
  explicit AnimationWorkletMessagingProxy(ExecutionContext*);

  void Trace(blink::Visitor*) override;

 private:
  ~AnimationWorkletMessagingProxy() override;

  std::unique_ptr<WorkerThread> CreateWorkerThread() override;
};

}  // namespace blink

#endif  // AnimationWorkletMessagingProxy_h
