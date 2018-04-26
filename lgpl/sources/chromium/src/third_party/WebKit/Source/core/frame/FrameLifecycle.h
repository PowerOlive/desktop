// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FrameLifecycle_h
#define FrameLifecycle_h

#include "base/macros.h"

namespace blink {

class FrameLifecycle {
 public:
  enum State {
    kAttached,
    kDetaching,
    kDetached,
  };

  FrameLifecycle();

  State GetState() const { return state_; }
  void AdvanceTo(State);

 private:
  State state_;

  DISALLOW_COPY_AND_ASSIGN(FrameLifecycle);
};

}  // namespace blink

#endif  // FrameLifecycle_h
