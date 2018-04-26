// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/locks/Lock.h"

#include "bindings/core/v8/ScriptPromise.h"
#include "bindings/core/v8/ScriptPromiseResolver.h"
#include "core/dom/ExecutionContext.h"
#include "modules/locks/LockManager.h"
#include "platform/bindings/ScriptState.h"

namespace blink {

namespace {
const char* kLockModeNameExclusive = "exclusive";
const char* kLockModeNameShared = "shared";
}  // namespace

class Lock::ThenFunction final : public ScriptFunction {
 public:
  enum ResolveType {
    Fulfilled,
    Rejected,
  };

  static v8::Local<v8::Function> CreateFunction(ScriptState* script_state,
                                                Lock* lock,
                                                ResolveType type) {
    ThenFunction* self = new ThenFunction(script_state, lock, type);
    return self->BindToV8Function();
  }

  virtual void Trace(blink::Visitor* visitor) {
    visitor->Trace(lock_);
    ScriptFunction::Trace(visitor);
  }

 private:
  ThenFunction(ScriptState* script_state, Lock* lock, ResolveType type)
      : ScriptFunction(script_state), lock_(lock), resolve_type_(type) {}

  ScriptValue Call(ScriptValue value) override {
    DCHECK(lock_);
    DCHECK(resolve_type_ == Fulfilled || resolve_type_ == Rejected);
    lock_->ReleaseIfHeld();
    if (resolve_type_ == Fulfilled)
      lock_->resolver_->Resolve(value);
    else
      lock_->resolver_->Reject(value);
    lock_ = nullptr;
    return value;
  }

  Member<Lock> lock_;
  ResolveType resolve_type_;
};

// static
Lock* Lock::Create(ScriptState* script_state,
                   const String& name,
                   mojom::blink::LockMode mode,
                   mojom::blink::LockHandlePtr handle,
                   LockManager* manager) {
  return new Lock(script_state, name, mode, std::move(handle), manager);
}

Lock::Lock(ScriptState* script_state,
           const String& name,
           mojom::blink::LockMode mode,
           mojom::blink::LockHandlePtr handle,
           LockManager* manager)
    : PausableObject(ExecutionContext::From(script_state)),
      name_(name),
      mode_(mode),
      handle_(std::move(handle)),
      manager_(manager) {
  PauseIfNeeded();
}

Lock::~Lock() = default;

String Lock::mode() const {
  return ModeToString(mode_);
}

void Lock::HoldUntil(ScriptPromise promise, ScriptPromiseResolver* resolver) {
  DCHECK(handle_.is_bound());
  DCHECK(!resolver_);

  ScriptState* script_state = resolver->GetScriptState();
  resolver_ = resolver;
  promise.Then(
      ThenFunction::CreateFunction(script_state, this, ThenFunction::Fulfilled),
      ThenFunction::CreateFunction(script_state, this, ThenFunction::Rejected));
}

// static
mojom::blink::LockMode Lock::StringToMode(const String& string) {
  if (string == kLockModeNameShared)
    return mojom::blink::LockMode::SHARED;
  if (string == kLockModeNameExclusive)
    return mojom::blink::LockMode::EXCLUSIVE;
  NOTREACHED();
  return mojom::blink::LockMode::SHARED;
}

// static
String Lock::ModeToString(mojom::blink::LockMode mode) {
  switch (mode) {
    case mojom::blink::LockMode::SHARED:
      return kLockModeNameShared;
    case mojom::blink::LockMode::EXCLUSIVE:
      return kLockModeNameExclusive;
  }
  NOTREACHED();
  return g_empty_string;
}

void Lock::ContextDestroyed(ExecutionContext* context) {
  ReleaseIfHeld();
}

void Lock::Trace(blink::Visitor* visitor) {
  PausableObject::Trace(visitor);
  ScriptWrappable::Trace(visitor);
  visitor->Trace(resolver_);
  visitor->Trace(manager_);
}

void Lock::ReleaseIfHeld() {
  if (handle_) {
    // Drop the mojo pipe; this releases the lock on the back end.
    handle_.reset();

    // Let the lock manager know that this instance can be collected.
    manager_->OnLockReleased(this);
  }
}

}  // namespace blink
