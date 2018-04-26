/*
 * Copyright (c) 2012 Google Inc. All rights reserved.
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

#include "platform/fonts/shaping/HarfBuzzFace.h"

#include <memory>

#include "build/build_config.h"
#include "platform/Histogram.h"
#include "platform/ResolutionUnits.h"
#include "platform/fonts/FontCache.h"
#include "platform/fonts/FontGlobalContext.h"
#include "platform/fonts/FontPlatformData.h"
#include "platform/fonts/SimpleFontData.h"
#include "platform/fonts/UnicodeRangeSet.h"
#include "platform/fonts/shaping/HarfBuzzFontCache.h"
#include "platform/fonts/shaping/HarfBuzzShaper.h"
#include "platform/fonts/skia/SkiaTextMetrics.h"
#include "platform/wtf/HashMap.h"
#include "platform/wtf/MathExtras.h"
#include "platform/wtf/PtrUtil.h"

#include <hb-ot.h>
#include <hb.h>
#if defined(OS_MACOSX)
#include <hb-coretext.h>
#endif

#include <SkPaint.h>
#include <SkPath.h>
#include <SkPoint.h>
#include <SkRect.h>
#include <SkStream.h>
#include <SkTypeface.h>

namespace blink {

void HbFontDeleter::operator()(hb_font_t* font) {
  if (font)
    hb_font_destroy(font);
}

void HbFaceDeleter::operator()(hb_face_t* face) {
  if (face)
    hb_face_destroy(face);
}

struct HbSetDeleter {
  void operator()(hb_set_t* set) {
    if (set)
      hb_set_destroy(set);
  }
};

using HbSetUniquePtr = std::unique_ptr<hb_set_t, HbSetDeleter>;

static scoped_refptr<HbFontCacheEntry> CreateHbFontCacheEntry(hb_face_t*);

HarfBuzzFace::HarfBuzzFace(FontPlatformData* platform_data, uint64_t unique_id)
    : platform_data_(platform_data), unique_id_(unique_id) {
  HarfBuzzFontCache::AddResult result =
      FontGlobalContext::GetHarfBuzzFontCache().insert(unique_id_, nullptr);
  if (result.is_new_entry) {
    HbFaceUniquePtr face(CreateFace());
    result.stored_value->value = CreateHbFontCacheEntry(face.get());
  }
  result.stored_value->value->AddRef();
  unscaled_font_ = result.stored_value->value->HbFont();
  harf_buzz_font_data_ = result.stored_value->value->HbFontData();
}

HarfBuzzFace::~HarfBuzzFace() {
  HarfBuzzFontCache::iterator result =
      FontGlobalContext::GetHarfBuzzFontCache().find(unique_id_);
  SECURITY_DCHECK(result != FontGlobalContext::GetHarfBuzzFontCache().end());
  DCHECK(!result.Get()->value->HasOneRef());
  result.Get()->value->Release();
  if (result.Get()->value->HasOneRef())
    FontGlobalContext::GetHarfBuzzFontCache().erase(unique_id_);
}

static hb_bool_t HarfBuzzGetGlyph(hb_font_t* hb_font,
                                  void* font_data,
                                  hb_codepoint_t unicode,
                                  hb_codepoint_t variation_selector,
                                  hb_codepoint_t* glyph,
                                  void* user_data) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);

  CHECK(hb_font_data);
  if (hb_font_data->range_set_ && !hb_font_data->range_set_->Contains(unicode))
    return false;

  return hb_font_get_glyph(hb_font_get_parent(hb_font), unicode,
                           variation_selector, glyph);
}

static hb_position_t HarfBuzzGetGlyphHorizontalAdvance(hb_font_t* hb_font,
                                                       void* font_data,
                                                       hb_codepoint_t glyph,
                                                       void* user_data) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);
  hb_position_t advance = 0;

  SkiaTextMetrics(&hb_font_data->paint_)
      .GetGlyphWidthForHarfBuzz(glyph, &advance);
  return advance;
}

static hb_bool_t HarfBuzzGetGlyphVerticalOrigin(hb_font_t* hb_font,
                                                void* font_data,
                                                hb_codepoint_t glyph,
                                                hb_position_t* x,
                                                hb_position_t* y,
                                                void* user_data) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);
  scoped_refptr<OpenTypeVerticalData> vertical_data =
      hb_font_data->VerticalData();
  if (!vertical_data)
    return false;

  float result[] = {0, 0};
  Glyph the_glyph = glyph;
  vertical_data->GetVerticalTranslationsForGlyphs(hb_font_data->paint_,
                                                  &the_glyph, 1, result);
  *x = SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(-result[0]);
  *y = SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(-result[1]);
  return true;
}

static hb_position_t HarfBuzzGetGlyphVerticalAdvance(hb_font_t* hb_font,
                                                     void* font_data,
                                                     hb_codepoint_t glyph,
                                                     void* user_data) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);
  scoped_refptr<OpenTypeVerticalData> vertical_data =
      hb_font_data->VerticalData();
  if (!vertical_data) {
    return SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(
        hb_font_data->height_fallback_);
  }

  Glyph the_glyph = glyph;
  float advance_height = -vertical_data->AdvanceHeight(the_glyph);
  return SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(
      SkFloatToScalar(advance_height));
}

static hb_position_t HarfBuzzGetGlyphHorizontalKerning(
    hb_font_t*,
    void* font_data,
    hb_codepoint_t left_glyph,
    hb_codepoint_t right_glyph,
    void*) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);
  if (hb_font_data->paint_.isVerticalText()) {
    // We don't support cross-stream kerning
    return 0;
  }

  SkTypeface* typeface = hb_font_data->paint_.getTypeface();

  const uint16_t glyphs[2] = {static_cast<uint16_t>(left_glyph),
                              static_cast<uint16_t>(right_glyph)};
  int32_t kerning_adjustments[1] = {0};

  if (typeface->getKerningPairAdjustments(glyphs, 2, kerning_adjustments)) {
    SkScalar upm = SkIntToScalar(typeface->getUnitsPerEm());
    SkScalar size = hb_font_data->paint_.getTextSize();
    return SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(
        SkIntToScalar(kerning_adjustments[0]) * size / upm);
  }

  return 0;
}

static hb_bool_t HarfBuzzGetGlyphExtents(hb_font_t* hb_font,
                                         void* font_data,
                                         hb_codepoint_t glyph,
                                         hb_glyph_extents_t* extents,
                                         void* user_data) {
  HarfBuzzFontData* hb_font_data =
      reinterpret_cast<HarfBuzzFontData*>(font_data);

  SkiaTextMetrics(&hb_font_data->paint_)
      .GetGlyphExtentsForHarfBuzz(glyph, extents);
  return true;
}

static inline bool TableHasSpace(hb_face_t* face,
                                 hb_set_t* glyphs,
                                 hb_tag_t tag,
                                 hb_codepoint_t space) {
  unsigned count = hb_ot_layout_table_get_lookup_count(face, tag);
  for (unsigned i = 0; i < count; i++) {
    hb_ot_layout_lookup_collect_glyphs(face, tag, i, glyphs, glyphs, glyphs,
                                       nullptr);
    if (hb_set_has(glyphs, space))
      return true;
  }
  return false;
}

static bool GetSpaceGlyph(hb_font_t* font, hb_codepoint_t& space) {
  return hb_font_get_nominal_glyph(font, kSpaceCharacter, &space);
}

bool HarfBuzzFace::HasSpaceInLigaturesOrKerning(TypesettingFeatures features) {
  const hb_codepoint_t kInvalidCodepoint = static_cast<hb_codepoint_t>(-1);
  hb_codepoint_t space = kInvalidCodepoint;

  HbSetUniquePtr glyphs(hb_set_create());

  // Check whether computing is needed and compute for gpos/gsub.
  if (features & kKerning &&
      harf_buzz_font_data_->space_in_gpos_ ==
          HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Unknown) {
    if (space == kInvalidCodepoint && !GetSpaceGlyph(unscaled_font_, space))
      return false;
    // Compute for gpos.
    hb_face_t* face = hb_font_get_face(unscaled_font_);
    DCHECK(face);
    harf_buzz_font_data_->space_in_gpos_ =
        hb_ot_layout_has_positioning(face) &&
                TableHasSpace(face, glyphs.get(), HB_OT_TAG_GPOS, space)
            ? HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Present
            : HarfBuzzFontData::SpaceGlyphInOpenTypeTables::NotPresent;
  }

  hb_set_clear(glyphs.get());

  if (features & kLigatures &&
      harf_buzz_font_data_->space_in_gsub_ ==
          HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Unknown) {
    if (space == kInvalidCodepoint && !GetSpaceGlyph(unscaled_font_, space))
      return false;
    // Compute for gpos.
    hb_face_t* face = hb_font_get_face(unscaled_font_);
    DCHECK(face);
    harf_buzz_font_data_->space_in_gsub_ =
        hb_ot_layout_has_substitution(face) &&
                TableHasSpace(face, glyphs.get(), HB_OT_TAG_GSUB, space)
            ? HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Present
            : HarfBuzzFontData::SpaceGlyphInOpenTypeTables::NotPresent;
  }

  return (features & kKerning &&
          harf_buzz_font_data_->space_in_gpos_ ==
              HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Present) ||
         (features & kLigatures &&
          harf_buzz_font_data_->space_in_gsub_ ==
              HarfBuzzFontData::SpaceGlyphInOpenTypeTables::Present);
}

static hb_font_funcs_t* HarfBuzzSkiaGetFontFuncs() {
  hb_font_funcs_t* funcs = FontGlobalContext::GetHarfBuzzFontFuncs();

  // We don't set callback functions which we can't support.
  // HarfBuzz will use the fallback implementation if they aren't set.
  if (!funcs) {
    funcs = hb_font_funcs_create();
    hb_font_funcs_set_glyph_func(funcs, HarfBuzzGetGlyph, nullptr, nullptr);
    hb_font_funcs_set_glyph_h_advance_func(
        funcs, HarfBuzzGetGlyphHorizontalAdvance, nullptr, nullptr);
    hb_font_funcs_set_glyph_h_kerning_func(
        funcs, HarfBuzzGetGlyphHorizontalKerning, nullptr, nullptr);
    hb_font_funcs_set_glyph_v_advance_func(
        funcs, HarfBuzzGetGlyphVerticalAdvance, nullptr, nullptr);
    hb_font_funcs_set_glyph_v_origin_func(funcs, HarfBuzzGetGlyphVerticalOrigin,
                                          nullptr, nullptr);
    hb_font_funcs_set_glyph_extents_func(funcs, HarfBuzzGetGlyphExtents,
                                         nullptr, nullptr);
    hb_font_funcs_make_immutable(funcs);
    FontGlobalContext::SetHarfBuzzFontFuncs(funcs);
  }
  DCHECK(funcs);
  return funcs;
}

static hb_blob_t* HarfBuzzSkiaGetTable(hb_face_t* face,
                                       hb_tag_t tag,
                                       void* user_data) {
  SkTypeface* typeface = reinterpret_cast<SkTypeface*>(user_data);

  const size_t table_size = typeface->getTableSize(tag);
  if (!table_size) {
    return nullptr;
  }

  char* buffer = reinterpret_cast<char*>(WTF::Partitions::FastMalloc(
      table_size, WTF_HEAP_PROFILER_TYPE_NAME(HarfBuzzFontData)));
  if (!buffer)
    return nullptr;
  size_t actual_size = typeface->getTableData(tag, 0, table_size, buffer);
  if (table_size != actual_size) {
    WTF::Partitions::FastFree(buffer);
    return nullptr;
  }
  return hb_blob_create(const_cast<char*>(buffer), table_size,
                        HB_MEMORY_MODE_WRITABLE, buffer,
                        WTF::Partitions::FastFree);
}

static void DeleteTypefaceStream(void* stream_asset_ptr) {
  SkStreamAsset* stream_asset =
      reinterpret_cast<SkStreamAsset*>(stream_asset_ptr);
  delete stream_asset;
}

hb_face_t* HarfBuzzFace::CreateFace() {
#if defined(OS_MACOSX)
  // hb_face_t needs to be instantiated using the CoreText constructor for
  // compatibility with AAT font, in which case HarfBuzz' CoreText backend is
  // used. If we encounter a FreeType backed SkTypeface, for variable fonts on
  // Mac OS < 10.12, follow the regular OpenType-only codepath below.
  if (platform_data_->CgFont()) {
    hb_face_t* face = hb_coretext_face_create(platform_data_->CgFont());
    DCHECK(face);
    return face;
  }
#endif
  hb_face_t* face = nullptr;

  DEFINE_THREAD_SAFE_STATIC_LOCAL(BooleanHistogram, zero_copy_success_histogram,
                                  ("Blink.Fonts.HarfBuzzFaceZeroCopyAccess"));
  SkTypeface* typeface = platform_data_->Typeface();
  CHECK(typeface);
  int ttc_index = 0;
  SkStreamAsset* typeface_stream = typeface->openStream(&ttc_index);
  if (typeface_stream && typeface_stream->getMemoryBase()) {
    std::unique_ptr<hb_blob_t, void (*)(hb_blob_t*)> face_blob(
        hb_blob_create(
            reinterpret_cast<const char*>(typeface_stream->getMemoryBase()),
            typeface_stream->getLength(), HB_MEMORY_MODE_READONLY,
            typeface_stream, DeleteTypefaceStream),
        hb_blob_destroy);
    face = hb_face_create(face_blob.get(), ttc_index);
  }

  // Fallback to table copies if there is no in-memory access.
  if (!face) {
    face = hb_face_create_for_tables(HarfBuzzSkiaGetTable,
                                     platform_data_->Typeface(), nullptr);
    zero_copy_success_histogram.Count(false);
  } else {
    zero_copy_success_histogram.Count(true);
  }

  DCHECK(face);
  return face;
}

scoped_refptr<HbFontCacheEntry> CreateHbFontCacheEntry(hb_face_t* face) {
  HbFontUniquePtr ot_font(hb_font_create(face));
  hb_ot_font_set_funcs(ot_font.get());
  // Creating a sub font means that non-available functions
  // are found from the parent.
  hb_font_t* unscaled_font = hb_font_create_sub_font(ot_font.get());
  scoped_refptr<HbFontCacheEntry> cache_entry =
      HbFontCacheEntry::Create(unscaled_font);
  hb_font_set_funcs(unscaled_font, HarfBuzzSkiaGetFontFuncs(),
                    cache_entry->HbFontData(), nullptr);
  return cache_entry;
}

static_assert(
    std::is_same<decltype(SkFontArguments::VariationPosition::Coordinate::axis),
                 decltype(hb_variation_t::tag)>::value &&
        std::is_same<
            decltype(SkFontArguments::VariationPosition::Coordinate::value),
            decltype(hb_variation_t::value)>::value &&
        sizeof(SkFontArguments::VariationPosition::Coordinate) ==
            sizeof(hb_variation_t),
    "Skia and HarfBuzz Variation parameter types must match in structure and "
    "size.");

hb_font_t* HarfBuzzFace::GetScaledFont(
    scoped_refptr<UnicodeRangeSet> range_set,
    VerticalLayoutCallbacks vertical_layout) const {
  PaintFont paint_font;
  platform_data_->SetupPaintFont(&paint_font);
  paint_font.SetTextEncoding(SkPaint::kGlyphID_TextEncoding);
  harf_buzz_font_data_->range_set_ = std::move(range_set);
  harf_buzz_font_data_->UpdateFallbackMetricsAndScale(
      *platform_data_, paint_font.ToSkPaint(), vertical_layout);

  int scale =
      SkiaTextMetrics::SkiaScalarToHarfBuzzPosition(platform_data_->size());
  hb_font_set_scale(unscaled_font_, scale, scale);
  hb_font_set_ptem(unscaled_font_, platform_data_->size() / kCssPixelsPerPoint);

  SkTypeface* typeface = harf_buzz_font_data_->paint_.getTypeface();
  int axis_count = typeface->getVariationDesignPosition(nullptr, 0);
  if (axis_count > 0) {
    Vector<SkFontArguments::VariationPosition::Coordinate> axis_values;
    axis_values.resize(axis_count);
    if (typeface->getVariationDesignPosition(axis_values.data(),
                                             axis_values.size()) > 0) {
      hb_font_set_variations(
          unscaled_font_, reinterpret_cast<hb_variation_t*>(axis_values.data()),
          axis_values.size());
    }
  }

  return unscaled_font_;
}

}  // namespace blink
