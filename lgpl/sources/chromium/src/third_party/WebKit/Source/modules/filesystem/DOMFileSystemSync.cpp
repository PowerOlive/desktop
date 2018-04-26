/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
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

#include "modules/filesystem/DOMFileSystemSync.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "bindings/core/v8/ExceptionState.h"
#include "core/fileapi/File.h"
#include "core/fileapi/FileError.h"
#include "modules/filesystem/DOMFilePath.h"
#include "modules/filesystem/DirectoryEntrySync.h"
#include "modules/filesystem/FileEntrySync.h"
#include "modules/filesystem/FileSystemCallbacks.h"
#include "modules/filesystem/FileWriterBaseCallback.h"
#include "modules/filesystem/FileWriterSync.h"
#include "platform/FileMetadata.h"
#include "public/platform/WebFileSystem.h"
#include "public/platform/WebFileSystemCallbacks.h"

namespace blink {

class FileWriterBase;

DOMFileSystemSync* DOMFileSystemSync::Create(DOMFileSystemBase* file_system) {
  return new DOMFileSystemSync(file_system->context_, file_system->name(),
                               file_system->GetType(), file_system->RootURL());
}

DOMFileSystemSync::DOMFileSystemSync(ExecutionContext* context,
                                     const String& name,
                                     FileSystemType type,
                                     const KURL& root_url)
    : DOMFileSystemBase(context, name, type, root_url),
      root_entry_(DirectoryEntrySync::Create(this, DOMFilePath::kRoot)) {}

DOMFileSystemSync::~DOMFileSystemSync() = default;

void DOMFileSystemSync::ReportError(ErrorCallbackBase* error_callback,
                                    FileError::ErrorCode file_error) {
  error_callback->Invoke(file_error);
}

DirectoryEntrySync* DOMFileSystemSync::root() {
  return root_entry_.Get();
}

namespace {

class CreateFileHelper final : public AsyncFileSystemCallbacks {
 public:
  class CreateFileResult : public GarbageCollected<CreateFileResult> {
   public:
    static CreateFileResult* Create() { return new CreateFileResult(); }

    bool failed_;
    int code_;
    Member<File> file_;

    void Trace(blink::Visitor* visitor) { visitor->Trace(file_); }

   private:
    CreateFileResult() : failed_(false), code_(0) {}
  };

  static std::unique_ptr<AsyncFileSystemCallbacks> Create(
      CreateFileResult* result,
      const String& name,
      const KURL& url,
      FileSystemType type) {
    return base::WrapUnique(static_cast<AsyncFileSystemCallbacks*>(
        new CreateFileHelper(result, name, url, type)));
  }

  void DidFail(int code) override {
    result_->failed_ = true;
    result_->code_ = code;
  }

  ~CreateFileHelper() override = default;

  void DidCreateSnapshotFile(const FileMetadata& metadata,
                             scoped_refptr<BlobDataHandle> snapshot) override {
    // We can't directly use the snapshot blob data handle because the content
    // type on it hasn't been set.  The |snapshot| param is here to provide a a
    // chain of custody thru thread bridging that is held onto until *after*
    // we've coined a File with a new handle that has the correct type set on
    // it. This allows the blob storage system to track when a temp file can and
    // can't be safely deleted.

    result_->file_ =
        DOMFileSystemBase::CreateFile(metadata, url_, type_, name_);
  }

  bool ShouldBlockUntilCompletion() const override { return true; }

 private:
  CreateFileHelper(CreateFileResult* result,
                   const String& name,
                   const KURL& url,
                   FileSystemType type)
      : result_(result), name_(name), url_(url), type_(type) {}

  Persistent<CreateFileResult> result_;
  String name_;
  KURL url_;
  FileSystemType type_;
};

}  // namespace

File* DOMFileSystemSync::CreateFile(const FileEntrySync* file_entry,
                                    ExceptionState& exception_state) {
  KURL file_system_url = CreateFileSystemURL(file_entry);
  CreateFileHelper::CreateFileResult* result(
      CreateFileHelper::CreateFileResult::Create());
  FileSystem()->CreateSnapshotFileAndReadMetadata(
      file_system_url, CreateFileHelper::Create(result, file_entry->name(),
                                                file_system_url, GetType()));
  if (result->failed_) {
    exception_state.ThrowDOMException(
        result->code_, "Could not create '" + file_entry->name() + "'.");
    return nullptr;
  }
  return result->file_.Get();
}

namespace {

class ReceiveFileWriterCallback final : public FileWriterBaseCallback {
 public:
  static ReceiveFileWriterCallback* Create() {
    return new ReceiveFileWriterCallback();
  }

  void handleEvent(FileWriterBase*) override {}

 private:
  ReceiveFileWriterCallback() = default;
};

class LocalErrorCallback final : public ErrorCallbackBase {
 public:
  static LocalErrorCallback* Create(FileError::ErrorCode& error_code) {
    return new LocalErrorCallback(error_code);
  }

  void Invoke(FileError::ErrorCode error) override {
    DCHECK_NE(error, FileError::kOK);
    error_code_ = error;
  }

 private:
  explicit LocalErrorCallback(FileError::ErrorCode& error_code)
      : error_code_(error_code) {}

  FileError::ErrorCode& error_code_;
};

}  // namespace

FileWriterSync* DOMFileSystemSync::CreateWriter(
    const FileEntrySync* file_entry,
    ExceptionState& exception_state) {
  DCHECK(file_entry);

  FileWriterSync* file_writer = FileWriterSync::Create();
  ReceiveFileWriterCallback* success_callback =
      ReceiveFileWriterCallback::Create();
  FileError::ErrorCode error_code = FileError::kOK;
  LocalErrorCallback* error_callback = LocalErrorCallback::Create(error_code);

  std::unique_ptr<AsyncFileSystemCallbacks> callbacks =
      FileWriterBaseCallbacks::Create(file_writer, success_callback,
                                      error_callback, context_);
  callbacks->SetShouldBlockUntilCompletion(true);

  FileSystem()->CreateFileWriter(CreateFileSystemURL(file_entry), file_writer,
                                 std::move(callbacks));
  if (error_code != FileError::kOK) {
    FileError::ThrowDOMException(exception_state, error_code);
    return nullptr;
  }
  return file_writer;
}

void DOMFileSystemSync::Trace(blink::Visitor* visitor) {
  visitor->Trace(root_entry_);
  DOMFileSystemBase::Trace(visitor);
}

}  // namespace blink
