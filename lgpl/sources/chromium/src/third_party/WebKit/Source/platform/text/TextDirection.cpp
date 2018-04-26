// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/text/TextDirection.h"

#include <ostream>

namespace blink {

std::ostream& operator<<(std::ostream& ostream, TextDirection direction) {
  return ostream << (IsLtr(direction) ? "LTR" : "RTL");
}

}  // namespace blink