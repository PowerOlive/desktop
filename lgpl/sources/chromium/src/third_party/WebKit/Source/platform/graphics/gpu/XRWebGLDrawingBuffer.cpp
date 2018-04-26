// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/graphics/gpu/XRWebGLDrawingBuffer.h"

#include "platform/graphics/AcceleratedStaticBitmapImage.h"
#include "platform/graphics/gpu/DrawingBuffer.h"
#include "platform/graphics/gpu/Extensions3DUtil.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace blink {

// Large parts of this file have been shamelessly borrowed from
// platform/graphics/gpu/DrawingBuffer.cpp and simplified where applicable due
// to the more narrow use case. It may make sense in the future to abstract out
// some of the common bits into a base class?

XRWebGLDrawingBuffer* XRWebGLDrawingBuffer::Create(
    DrawingBuffer* drawing_buffer,
    GLuint framebuffer,
    const IntSize& size,
    bool want_alpha_channel,
    bool want_depth_buffer,
    bool want_stencil_buffer,
    bool want_antialiasing,
    bool want_multiview) {
  DCHECK(drawing_buffer);

  // Don't proceeed if the context is already lost.
  if (drawing_buffer->destroyed())
    return nullptr;

  gpu::gles2::GLES2Interface* gl = drawing_buffer->ContextGL();

  std::unique_ptr<Extensions3DUtil> extensions_util =
      Extensions3DUtil::Create(gl);
  if (!extensions_util->IsValid()) {
    return nullptr;
  }

  DCHECK(extensions_util->SupportsExtension("GL_OES_packed_depth_stencil"));
  extensions_util->EnsureExtensionEnabled("GL_OES_packed_depth_stencil");
  bool multisample_supported =
      want_antialiasing &&
      (extensions_util->SupportsExtension(
           "GL_CHROMIUM_framebuffer_multisample") ||
       extensions_util->SupportsExtension(
           "GL_EXT_multisampled_render_to_texture")) &&
      extensions_util->SupportsExtension("GL_OES_rgb8_rgba8");
  if (multisample_supported) {
    extensions_util->EnsureExtensionEnabled("GL_OES_rgb8_rgba8");
    if (extensions_util->SupportsExtension(
            "GL_CHROMIUM_framebuffer_multisample")) {
      extensions_util->EnsureExtensionEnabled(
          "GL_CHROMIUM_framebuffer_multisample");
    } else {
      extensions_util->EnsureExtensionEnabled(
          "GL_EXT_multisampled_render_to_texture");
    }
  }
  bool discard_framebuffer_supported =
      extensions_util->SupportsExtension("GL_EXT_discard_framebuffer");
  if (discard_framebuffer_supported)
    extensions_util->EnsureExtensionEnabled("GL_EXT_discard_framebuffer");

  // TODO(bajones): Support multiview.
  bool multiview_supported = false;

  XRWebGLDrawingBuffer* xr_drawing_buffer = new XRWebGLDrawingBuffer(
      drawing_buffer, framebuffer, discard_framebuffer_supported,
      want_alpha_channel, want_depth_buffer, want_stencil_buffer,
      multiview_supported);
  if (!xr_drawing_buffer->Initialize(size, multisample_supported,
                                     multiview_supported)) {
    DLOG(ERROR) << "XRWebGLDrawingBuffer Initialization Failed";
    return nullptr;
  }

  return xr_drawing_buffer;
}

XRWebGLDrawingBuffer::XRWebGLDrawingBuffer(DrawingBuffer* drawing_buffer,
                                           GLuint framebuffer,
                                           bool discard_framebuffer_supported,
                                           bool want_alpha_channel,
                                           bool want_depth_buffer,
                                           bool want_stencil_buffer,
                                           bool multiview_supported)
    : drawing_buffer_(drawing_buffer),
      framebuffer_(framebuffer),
      discard_framebuffer_supported_(discard_framebuffer_supported),
      depth_(want_depth_buffer),
      stencil_(want_stencil_buffer),
      alpha_(want_alpha_channel),
      multiview_(false) {}

// TODO(bajones): The GL resources allocated in this function are leaking. Add
// a way to clean up the buffers when the layer is GCed or the session ends.
bool XRWebGLDrawingBuffer::Initialize(const IntSize& size,
                                      bool use_multisampling,
                                      bool use_multiview) {
  gpu::gles2::GLES2Interface* gl = drawing_buffer_->ContextGL();

  std::unique_ptr<Extensions3DUtil> extensions_util =
      Extensions3DUtil::Create(gl);

  // Check context capabilities
  int max_sample_count = 0;
  anti_aliasing_mode_ = kNone;
  if (use_multisampling) {
    gl->GetIntegerv(GL_MAX_SAMPLES_ANGLE, &max_sample_count);
    anti_aliasing_mode_ = kMSAAExplicitResolve;
    if (extensions_util->SupportsExtension(
            "GL_EXT_multisampled_render_to_texture")) {
      anti_aliasing_mode_ = kMSAAImplicitResolve;
    } else if (extensions_util->SupportsExtension(
                   "GL_CHROMIUM_screen_space_antialiasing")) {
      anti_aliasing_mode_ = kScreenSpaceAntialiasing;
    }
  }

  storage_texture_supported_ =
      (drawing_buffer_->webgl_version() > DrawingBuffer::kWebGL1 ||
       extensions_util->SupportsExtension("GL_EXT_texture_storage")) &&
      anti_aliasing_mode_ == kScreenSpaceAntialiasing;
  sample_count_ = std::min(4, max_sample_count);

  Resize(size);

  // It's possible that the drawing buffer allocation provokes a context loss,
  // so check again just in case.
  if (gl->GetGraphicsResetStatusKHR() != GL_NO_ERROR) {
    return false;
  }

  return true;
}

bool XRWebGLDrawingBuffer::ContextLost() {
  return drawing_buffer_->destroyed();
}

void XRWebGLDrawingBuffer::Resize(const IntSize& new_size) {
  // Ensure we always have at least a 1x1 buffer
  IntSize adjusted_size(std::max(1, new_size.Width()),
                        std::max(1, new_size.Height()));

  if (adjusted_size == size_)
    return;

  // Don't attempt to resize if the context is lost.
  if (ContextLost())
    return;

  gpu::gles2::GLES2Interface* gl = drawing_buffer_->ContextGL();

  size_ = adjusted_size;

  gl->BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  // Provide a depth and/or stencil buffer if requested.
  if (depth_ || stencil_) {
    if (depth_stencil_buffer_) {
      gl->DeleteRenderbuffers(1, &depth_stencil_buffer_);
      depth_stencil_buffer_ = 0;
    }
    gl->GenRenderbuffers(1, &depth_stencil_buffer_);
    gl->BindRenderbuffer(GL_RENDERBUFFER, depth_stencil_buffer_);

    if (anti_aliasing_mode_ == kMSAAImplicitResolve) {
      gl->RenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, sample_count_,
                                            GL_DEPTH24_STENCIL8_OES,
                                            size_.Width(), size_.Height());
    } else if (anti_aliasing_mode_ == kMSAAExplicitResolve) {
      gl->RenderbufferStorageMultisampleCHROMIUM(GL_RENDERBUFFER, sample_count_,
                                                 GL_DEPTH24_STENCIL8_OES,
                                                 size_.Width(), size_.Height());
    } else {
      gl->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES,
                              size_.Width(), size_.Height());
    }

    gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, depth_stencil_buffer_);
  }

  if (WantExplicitResolve()) {
    // If we're doing an explicit multisample resolve use the main framebuffer
    // as the multisample target and resolve into resolved_fbo_ when needed.
    GLenum multisample_format = alpha_ ? GL_RGBA8_OES : GL_RGB8_OES;

    if (multisample_renderbuffer_) {
      gl->DeleteRenderbuffers(1, &multisample_renderbuffer_);
      multisample_renderbuffer_ = 0;
    }

    gl->GenRenderbuffers(1, &multisample_renderbuffer_);
    gl->BindRenderbuffer(GL_RENDERBUFFER, multisample_renderbuffer_);
    gl->RenderbufferStorageMultisampleCHROMIUM(GL_RENDERBUFFER, sample_count_,
                                               multisample_format,
                                               size_.Width(), size_.Height());

    gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, multisample_renderbuffer_);

    // Now bind the resolve target framebuffer to attach the color textures to.
    if (!resolved_framebuffer_) {
      gl->GenFramebuffers(1, &resolved_framebuffer_);
    }
    gl->BindFramebuffer(GL_FRAMEBUFFER, resolved_framebuffer_);
  }

  if (back_color_buffer_) {
    gl->DeleteTextures(1, &back_color_buffer_);
    back_color_buffer_ = 0;
  }
  if (front_color_buffer_) {
    gl->DeleteTextures(1, &front_color_buffer_);
    front_color_buffer_ = 0;
  }

  back_color_buffer_ = CreateColorBuffer();
  front_color_buffer_ = CreateColorBuffer();

  if (anti_aliasing_mode_ == kMSAAImplicitResolve) {
    gl->FramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, back_color_buffer_, 0,
                                           sample_count_);
  } else {
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, back_color_buffer_, 0);
  }

  if (gl->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    DLOG(ERROR) << "Framebuffer incomplete";
  }

  DrawingBuffer::Client* client = drawing_buffer_->client();
  client->DrawingBufferClientRestoreRenderbufferBinding();
  client->DrawingBufferClientRestoreFramebufferBinding();
}

GLuint XRWebGLDrawingBuffer::CreateColorBuffer() {
  gpu::gles2::GLES2Interface* gl = drawing_buffer_->ContextGL();

  GLuint texture_id = 0;
  gl->GenTextures(1, &texture_id);
  gl->BindTexture(GL_TEXTURE_2D, texture_id);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (storage_texture_supported_) {
    GLenum internal_storage_format = alpha_ ? GL_RGBA8 : GL_RGB8;
    gl->TexStorage2DEXT(GL_TEXTURE_2D, 1, internal_storage_format,
                        size_.Width(), size_.Height());
  } else {
    GLenum gl_format = alpha_ ? GL_RGBA : GL_RGB;
    gl->TexImage2D(GL_TEXTURE_2D, 0, gl_format, size_.Width(), size_.Height(),
                   0, gl_format, GL_UNSIGNED_BYTE, nullptr);
  }

  DrawingBuffer::Client* client = drawing_buffer_->client();
  client->DrawingBufferClientRestoreTexture2DBinding();

  return texture_id;
}

bool XRWebGLDrawingBuffer::WantExplicitResolve() const {
  return anti_aliasing_mode_ == kMSAAExplicitResolve;
}

// Swap the front and back buffers. After this call the front buffer should
// contain the previously rendered content, resolved from the multisample
// renderbuffer if needed.
void XRWebGLDrawingBuffer::SwapColorBuffers() {
  gpu::gles2::GLES2Interface* gl = drawing_buffer_->ContextGL();

  DrawingBuffer::Client* client = drawing_buffer_->client();

  // Resolve multisample buffers if needed
  if (WantExplicitResolve()) {
    gl->BindFramebuffer(GL_READ_FRAMEBUFFER_ANGLE, framebuffer_);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER_ANGLE, resolved_framebuffer_);
    gl->Disable(GL_SCISSOR_TEST);

    int width = size_.Width();
    int height = size_.Height();
    // Use NEAREST, because there is no scale performed during the blit.
    gl->BlitFramebufferCHROMIUM(0, 0, width, height, 0, 0, width, height,
                                GL_COLOR_BUFFER_BIT, GL_NEAREST);

    gl->BindFramebuffer(GL_FRAMEBUFFER, resolved_framebuffer_);

    client->DrawingBufferClientRestoreScissorTest();
  } else {
    gl->BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    if (anti_aliasing_mode_ == kScreenSpaceAntialiasing)
      gl->ApplyScreenSpaceAntialiasingCHROMIUM();
  }

  // Swap buffers
  GLuint tmp = back_color_buffer_;

  if (front_color_buffer_) {
    back_color_buffer_ = front_color_buffer_;
  } else {
    back_color_buffer_ = CreateColorBuffer();
  }

  front_color_buffer_ = tmp;

  if (anti_aliasing_mode_ == kMSAAImplicitResolve) {
    gl->FramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, back_color_buffer_, 0,
                                           sample_count_);
  } else {
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, back_color_buffer_, 0);
  }

  if (discard_framebuffer_supported_) {
    const GLenum kAttachments[3] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT,
                                    GL_STENCIL_ATTACHMENT};
    gl->DiscardFramebufferEXT(GL_FRAMEBUFFER, 3, kAttachments);
  }

  client->DrawingBufferClientRestoreFramebufferBinding();
}

scoped_refptr<StaticBitmapImage>
XRWebGLDrawingBuffer::TransferToStaticBitmapImage() {
  gpu::Mailbox mailbox;
  gpu::SyncToken sync_token;
  bool success = false;
  GLuint texture_id = 0;

  // Ensure the context isn't lost before continuing.
  if (!ContextLost()) {
    gpu::gles2::GLES2Interface* gl = drawing_buffer_->ContextGL();

    SwapColorBuffers();

    gl->GenMailboxCHROMIUM(mailbox.name);
    gl->ProduceTextureDirectCHROMIUM(front_color_buffer_, mailbox.name);
    gl->GenUnverifiedSyncTokenCHROMIUM(sync_token.GetData());

    // This should only fail if the context is lost during the buffer swap.
    if (sync_token.HasData()) {
      // Once we place the texture in the StaticBitmapImage it's effectively
      // gone for good. So we'll null out the front_color_buffer_ here to ensure
      // that a new one is created on the next swap.
      texture_id = front_color_buffer_;
      front_color_buffer_ = 0;
      success = true;
    }
  }

  if (!success) {
    // If we can't get a mailbox, return an transparent black ImageBitmap.
    // The only situation in which this could happen is when two or more calls
    // to transferToImageBitmap are made back-to-back, or when the context gets
    // lost.
    sk_sp<SkSurface> surface =
        SkSurface::MakeRasterN32Premul(size_.Width(), size_.Height());
    return StaticBitmapImage::Create(surface->makeImageSnapshot());
  }

  return AcceleratedStaticBitmapImage::CreateFromWebGLContextImage(
      mailbox, sync_token, texture_id,
      drawing_buffer_->ContextProviderWeakPtr(), size_);
}

}  // namespace blink
