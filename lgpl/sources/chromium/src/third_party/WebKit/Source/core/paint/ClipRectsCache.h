// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ClipRectsCache_h
#define ClipRectsCache_h

#include "core/paint/ClipRects.h"

#if DCHECK_IS_ON()
#include "platform/scroll/ScrollTypes.h"  // For OverlayScrollbarClipBehavior.
#endif

namespace blink {

class PaintLayer;

enum ClipRectsCacheSlot {
  // Relative to the LayoutView's layer. Used for compositing overlap testing.
  // TODO(bokan): Overlap testing currently ignores the clip on the root layer.
  // Overlap testing has some bugs when inside non-root layers which extend to
  // the root layer when root-layer-scrolling is turned on unless we do this.
  // crbug.com/783532.
  kAbsoluteClipRectsIgnoringViewportClip,

  // Relative to painting ancestor. Used for SPv1 compositing.
  kPaintingClipRects,
  kPaintingClipRectsIgnoringOverflowClip,

  kNumberOfClipRectsCacheSlots,
  kUncachedClipRects,
};

class ClipRectsCache {
  USING_FAST_MALLOC(ClipRectsCache);

 public:
  struct Entry {
    Entry()
        : root(nullptr)
#if DCHECK_IS_ON()
          ,
          overlay_scrollbar_clip_behavior(kIgnorePlatformOverlayScrollbarSize)
#endif
    {
    }
    const PaintLayer* root;
    scoped_refptr<ClipRects> clip_rects;
#if DCHECK_IS_ON()
    OverlayScrollbarClipBehavior overlay_scrollbar_clip_behavior;
#endif
  };
  Entry& Get(ClipRectsCacheSlot slot) {
    DCHECK(slot < kNumberOfClipRectsCacheSlots);
    return entries_[slot];
  }
  void Clear(ClipRectsCacheSlot slot) {
    DCHECK(slot < kNumberOfClipRectsCacheSlots);
    entries_[slot] = Entry();
  }

 private:
  Entry entries_[kNumberOfClipRectsCacheSlots];
};

}  // namespace blink

#endif  // ClipRectsCache_h
