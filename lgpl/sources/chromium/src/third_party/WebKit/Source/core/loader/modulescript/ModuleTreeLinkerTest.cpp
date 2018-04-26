// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/loader/modulescript/ModuleTreeLinker.h"

#include "bindings/core/v8/ScriptModule.h"
#include "bindings/core/v8/V8BindingForCore.h"
#include "bindings/core/v8/V8BindingForTesting.h"
#include "core/loader/modulescript/ModuleScriptFetchRequest.h"
#include "core/loader/modulescript/ModuleTreeLinkerRegistry.h"
#include "core/script/Modulator.h"
#include "core/script/ModuleScript.h"
#include "core/testing/DummyModulator.h"
#include "core/testing/DummyPageHolder.h"
#include "platform/bindings/ScriptState.h"
#include "platform/bindings/V8ThrowException.h"
#include "platform/heap/Handle.h"
#include "platform/testing/UnitTestHelpers.h"
#include "platform/weborigin/KURL.h"
#include "platform/wtf/text/StringBuilder.h"
#include "public/platform/Platform.h"
#include "public/platform/scheduler/renderer/renderer_scheduler.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace blink {

namespace {

class TestModuleTreeClient final : public ModuleTreeClient {
 public:
  TestModuleTreeClient() = default;

  void Trace(blink::Visitor* visitor) override {
    visitor->Trace(module_script_);
    ModuleTreeClient::Trace(visitor);
  }

  void NotifyModuleTreeLoadFinished(ModuleScript* module_script) override {
    was_notify_finished_ = true;
    module_script_ = module_script;
  }

  bool WasNotifyFinished() const { return was_notify_finished_; }
  ModuleScript* GetModuleScript() { return module_script_; }

 private:
  bool was_notify_finished_ = false;
  Member<ModuleScript> module_script_;
};

}  // namespace

class ModuleTreeLinkerTestModulator final : public DummyModulator {
 public:
  ModuleTreeLinkerTestModulator(scoped_refptr<ScriptState> script_state)
      : script_state_(std::move(script_state)) {}
  ~ModuleTreeLinkerTestModulator() override = default;

  void Trace(blink::Visitor*) override;

  enum class ResolveResult { kFailure, kSuccess };

  // Resolve last |Modulator::FetchSingle()| call.
  ModuleScript* ResolveSingleModuleScriptFetch(
      const KURL& url,
      const Vector<String>& dependency_module_specifiers,
      bool parse_error = false) {
    ScriptState::Scope scope(script_state_.get());

    StringBuilder source_text;
    Vector<ModuleRequest> dependency_module_requests;
    dependency_module_requests.ReserveInitialCapacity(
        dependency_module_specifiers.size());
    for (const auto& specifier : dependency_module_specifiers) {
      dependency_module_requests.emplace_back(specifier,
                                              TextPosition::MinimumPosition());
      source_text.Append("import '");
      source_text.Append(specifier);
      source_text.Append("';\n");
    }
    source_text.Append("export default 'grapes';");

    ScriptModule script_module = ScriptModule::Compile(
        script_state_->GetIsolate(), source_text.ToString(), url, url,
        ScriptFetchOptions(), kSharableCrossOrigin,
        TextPosition::MinimumPosition(), ASSERT_NO_EXCEPTION);
    auto* module_script = ModuleScript::CreateForTest(this, script_module, url);
    auto result_request = dependency_module_requests_map_.insert(
        script_module, dependency_module_requests);
    EXPECT_TRUE(result_request.is_new_entry);
    auto result_map = module_map_.insert(url, module_script);
    EXPECT_TRUE(result_map.is_new_entry);

    if (parse_error) {
      v8::Local<v8::Value> error = V8ThrowException::CreateError(
          script_state_->GetIsolate(), "Parse failure.");
      module_script->SetParseErrorAndClearRecord(
          ScriptValue(script_state_.get(), error));
    }

    EXPECT_TRUE(pending_clients_.Contains(url));
    pending_clients_.Take(url)->NotifyModuleLoadFinished(module_script);

    return module_script;
  }

  void ResolveDependentTreeFetch(const KURL& url, ResolveResult result) {
    ResolveSingleModuleScriptFetch(url, Vector<String>(),
                                   result == ResolveResult::kFailure);
  }

  void SetInstantiateShouldFail(bool b) { instantiate_should_fail_ = b; }

  bool HasInstantiated(ModuleScript* module_script) const {
    return instantiated_records_.Contains(module_script->Record());
  }

 private:
  // Implements Modulator:

  ReferrerPolicy GetReferrerPolicy() override { return kReferrerPolicyDefault; }
  ScriptState* GetScriptState() override { return script_state_.get(); }

  void FetchSingle(const ModuleScriptFetchRequest& request,
                   ModuleGraphLevel,
                   SingleModuleClient* client) override {
    EXPECT_FALSE(pending_clients_.Contains(request.Url()));
    pending_clients_.Set(request.Url(), client);
  }

  ModuleScript* GetFetchedModuleScript(const KURL& url) override {
    const auto& it = module_map_.find(url);
    if (it == module_map_.end())
      return nullptr;

    return it->value;
  }

  ScriptValue InstantiateModule(ScriptModule record) override {
    if (instantiate_should_fail_) {
      ScriptState::Scope scope(script_state_.get());
      v8::Local<v8::Value> error = V8ThrowException::CreateError(
          script_state_->GetIsolate(), "Instantiation failure.");
      return ScriptValue(script_state_.get(), error);
    }
    instantiated_records_.insert(record);
    return ScriptValue();
  }

  Vector<ModuleRequest> ModuleRequestsFromScriptModule(
      ScriptModule script_module) override {
    if (script_module.IsNull())
      return Vector<ModuleRequest>();

    const auto& it = dependency_module_requests_map_.find(script_module);
    if (it == dependency_module_requests_map_.end())
      return Vector<ModuleRequest>();

    return it->value;
  }

  scoped_refptr<ScriptState> script_state_;
  HeapHashMap<KURL, Member<SingleModuleClient>> pending_clients_;
  HashMap<ScriptModule, Vector<ModuleRequest>> dependency_module_requests_map_;
  HeapHashMap<KURL, Member<ModuleScript>> module_map_;
  HashSet<ScriptModule> instantiated_records_;
  bool instantiate_should_fail_ = false;
};

void ModuleTreeLinkerTestModulator::Trace(blink::Visitor* visitor) {
  visitor->Trace(pending_clients_);
  visitor->Trace(module_map_);
  DummyModulator::Trace(visitor);
}

class ModuleTreeLinkerTest : public ::testing::Test {
  DISALLOW_COPY_AND_ASSIGN(ModuleTreeLinkerTest);

 public:
  ModuleTreeLinkerTest() = default;
  void SetUp() override;

  ModuleTreeLinkerTestModulator* GetModulator() { return modulator_.Get(); }

 protected:
  std::unique_ptr<DummyPageHolder> dummy_page_holder_;
  Persistent<ModuleTreeLinkerTestModulator> modulator_;
};

void ModuleTreeLinkerTest::SetUp() {
  dummy_page_holder_ = DummyPageHolder::Create(IntSize(500, 500));
  scoped_refptr<ScriptState> script_state =
      ToScriptStateForMainWorld(&dummy_page_holder_->GetFrame());
  modulator_ = new ModuleTreeLinkerTestModulator(script_state);
}

TEST_F(ModuleTreeLinkerTest, FetchTreeNoDeps) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/root.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(url, {});
  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(GetModulator()->HasInstantiated(client->GetModuleScript()));
}

TEST_F(ModuleTreeLinkerTest, FetchTreeInstantiationFailure) {
  GetModulator()->SetInstantiateShouldFail(true);

  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/root.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(url, {});

  // Modulator::InstantiateModule() fails here, as
  // we SetInstantiateShouldFail(true) earlier.

  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(client->GetModuleScript()->HasErrorToRethrow())
      << "Expected errored module script but got "
      << *client->GetModuleScript();
}

TEST_F(ModuleTreeLinkerTest, FetchTreeWithSingleDependency) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/root.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(url, {"./dep1.js"});
  EXPECT_FALSE(client->WasNotifyFinished());

  KURL url_dep1("http://example.com/dep1.js");

  GetModulator()->ResolveDependentTreeFetch(
      url_dep1, ModuleTreeLinkerTestModulator::ResolveResult::kSuccess);
  EXPECT_TRUE(client->WasNotifyFinished());

  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(GetModulator()->HasInstantiated(client->GetModuleScript()));
}

TEST_F(ModuleTreeLinkerTest, FetchTreeWith3Deps) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/root.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(
      url, {"./dep1.js", "./dep2.js", "./dep3.js"});
  EXPECT_FALSE(client->WasNotifyFinished());

  Vector<KURL> url_deps;
  for (int i = 1; i <= 3; ++i) {
    StringBuilder url_dep_str;
    url_dep_str.Append("http://example.com/dep");
    url_dep_str.AppendNumber(i);
    url_dep_str.Append(".js");

    KURL url_dep(url_dep_str.ToString());
    url_deps.push_back(url_dep);
  }

  for (const auto& url_dep : url_deps) {
    EXPECT_FALSE(client->WasNotifyFinished());
    GetModulator()->ResolveDependentTreeFetch(
        url_dep, ModuleTreeLinkerTestModulator::ResolveResult::kSuccess);
  }

  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(GetModulator()->HasInstantiated(client->GetModuleScript()));
}

TEST_F(ModuleTreeLinkerTest, FetchTreeWith3Deps1Fail) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/root.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(
      url, {"./dep1.js", "./dep2.js", "./dep3.js"});
  EXPECT_FALSE(client->WasNotifyFinished());

  Vector<KURL> url_deps;
  for (int i = 1; i <= 3; ++i) {
    StringBuilder url_dep_str;
    url_dep_str.Append("http://example.com/dep");
    url_dep_str.AppendNumber(i);
    url_dep_str.Append(".js");

    KURL url_dep(url_dep_str.ToString());
    url_deps.push_back(url_dep);
  }

  for (const auto& url_dep : url_deps) {
    SCOPED_TRACE(url_dep.GetString());
  }

  auto url_dep = url_deps.back();
  url_deps.pop_back();
  GetModulator()->ResolveDependentTreeFetch(
      url_dep, ModuleTreeLinkerTestModulator::ResolveResult::kSuccess);
  EXPECT_FALSE(client->WasNotifyFinished());
  url_dep = url_deps.back();
  url_deps.pop_back();
  GetModulator()->ResolveDependentTreeFetch(
      url_dep, ModuleTreeLinkerTestModulator::ResolveResult::kFailure);

  // TODO(kouhei): This may not hold once we implement early failure reporting.
  EXPECT_FALSE(client->WasNotifyFinished());

  // Check below doesn't crash.
  url_dep = url_deps.back();
  url_deps.pop_back();
  GetModulator()->ResolveDependentTreeFetch(
      url_dep, ModuleTreeLinkerTestModulator::ResolveResult::kSuccess);
  EXPECT_TRUE(url_deps.IsEmpty());

  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_FALSE(client->GetModuleScript()->HasParseError());
  EXPECT_TRUE(client->GetModuleScript()->HasErrorToRethrow());
}

TEST_F(ModuleTreeLinkerTest, FetchDependencyTree) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/depth1.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(url, {"./depth2.js"});

  KURL url_dep2("http://example.com/depth2.js");

  GetModulator()->ResolveDependentTreeFetch(
      url_dep2, ModuleTreeLinkerTestModulator::ResolveResult::kSuccess);

  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(GetModulator()->HasInstantiated(client->GetModuleScript()));
}

TEST_F(ModuleTreeLinkerTest, FetchDependencyOfCyclicGraph) {
  ModuleTreeLinkerRegistry* registry = ModuleTreeLinkerRegistry::Create();

  KURL url("http://example.com/a.js");
  ModuleScriptFetchRequest module_request(url, kReferrerPolicyDefault,
                                          ScriptFetchOptions());
  TestModuleTreeClient* client = new TestModuleTreeClient;
  registry->Fetch(module_request, GetModulator(), client);

  EXPECT_FALSE(client->WasNotifyFinished())
      << "ModuleTreeLinker should always finish asynchronously.";
  EXPECT_FALSE(client->GetModuleScript());

  GetModulator()->ResolveSingleModuleScriptFetch(url, {"./a.js"});

  EXPECT_TRUE(client->WasNotifyFinished());
  ASSERT_TRUE(client->GetModuleScript());
  EXPECT_TRUE(GetModulator()->HasInstantiated(client->GetModuleScript()));
}

}  // namespace blink
