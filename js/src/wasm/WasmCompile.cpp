/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCompile.h"

#include "mozilla/Maybe.h"
#include "mozilla/Unused.h"

#include "jsprf.h"

#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBinaryIterator.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

static bool
DecodeFunctionBody(Decoder& d, ModuleGenerator& mg, uint32_t funcIndex)
{
    uint32_t bodySize;
    if (!d.readVarU32(&bodySize))
        return d.fail("expected number of function body bytes");

    const size_t offsetInModule = d.currentOffset();

    // Skip over the function body; we'll validate it later.
    const uint8_t* bodyBegin;
    if (!d.readBytes(bodySize, &bodyBegin))
        return d.fail("function body length too big");

    FunctionGenerator fg;
    if (!mg.startFuncDef(offsetInModule, &fg))
        return false;

    if (!fg.bytes().resize(bodySize))
        return false;

    memcpy(fg.bytes().begin(), bodyBegin, bodySize);

    return mg.finishFuncDef(funcIndex, &fg);
}

static bool
DecodeCodeSection(Decoder& d, ModuleGenerator& mg)
{
    uint32_t sectionStart, sectionSize;
    if (!d.startSection(SectionId::Code, &mg.mutableEnv(), &sectionStart, &sectionSize, "code"))
        return false;

    if (!mg.startFuncDefs())
        return false;

    if (sectionStart == Decoder::NotStarted) {
        if (mg.env().numFuncDefs() != 0)
            return d.fail("expected function bodies");

        return mg.finishFuncDefs();
    }

    uint32_t numFuncDefs;
    if (!d.readVarU32(&numFuncDefs))
        return d.fail("expected function body count");

    if (numFuncDefs != mg.env().numFuncDefs())
        return d.fail("function body count does not match function signature count");

    for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
        if (!DecodeFunctionBody(d, mg, mg.env().numFuncImports() + funcDefIndex))
            return false;
    }

    if (!d.finishSection(sectionStart, sectionSize, "code"))
        return false;

    return mg.finishFuncDefs();
}

bool
CompileArgs::initFromContext(JSContext* cx, ScriptedCaller&& scriptedCaller)
{
    baselineEnabled = cx->options().wasmBaseline();

    // For sanity's sake, just use Ion if both compilers are disabled.
    ionEnabled = cx->options().wasmIon() || !cx->options().wasmBaseline();

    // Debug information such as source view or debug traps will require
    // additional memory and permanently stay in baseline code, so we try to
    // only enable it when a developer actually cares: when the debugger tab
    // is open.
    debugEnabled = cx->compartment()->debuggerObservesAsmJS();

    this->scriptedCaller = Move(scriptedCaller);
    return assumptions.initBuildIdFromContext(cx);
}

static void
CompilerAvailability(ModuleKind kind, const CompileArgs& args, bool* baselineEnabled,
                     bool* debugEnabled, bool* ionEnabled)
{
    bool baselinePossible = kind == ModuleKind::Wasm && BaselineCanCompile();
    *baselineEnabled = baselinePossible && args.baselineEnabled;
    *debugEnabled = baselinePossible && args.debugEnabled;
    *ionEnabled = args.ionEnabled;

    // Default to Ion if necessary: We will never get to this point on platforms
    // that don't have Ion at all, so this can happen if the user has disabled
    // both compilers or if she has disabled Ion but baseline can't compile the
    // code.

    if (!(*baselineEnabled || *ionEnabled))
        *ionEnabled = true;
}

static bool
BackgroundWorkPossible()
{
    return CanUseExtraThreads() && HelperThreadState().cpuCount > 1;
}

bool
wasm::GetDebugEnabled(const CompileArgs& args, ModuleKind kind)
{
    bool baselineEnabled, debugEnabled, ionEnabled;
    CompilerAvailability(kind, args, &baselineEnabled, &debugEnabled, &ionEnabled);

    return debugEnabled;
}

wasm::CompileMode
wasm::GetInitialCompileMode(const CompileArgs& args, ModuleKind kind)
{
    bool baselineEnabled, debugEnabled, ionEnabled;
    CompilerAvailability(kind, args, &baselineEnabled, &debugEnabled, &ionEnabled);

    return BackgroundWorkPossible() && baselineEnabled && ionEnabled && !debugEnabled
           ? CompileMode::Tier1
           : CompileMode::Once;
}

wasm::Tier
wasm::GetTier(const CompileArgs& args, CompileMode compileMode, ModuleKind kind)
{
    bool baselineEnabled, debugEnabled, ionEnabled;
    CompilerAvailability(kind, args, &baselineEnabled, &debugEnabled, &ionEnabled);

    switch (compileMode) {
      case CompileMode::Tier1:
        MOZ_ASSERT(baselineEnabled);
        return Tier::Baseline;

      case CompileMode::Tier2:
        MOZ_ASSERT(ionEnabled);
        return Tier::Ion;

      case CompileMode::Once:
        return (debugEnabled || !ionEnabled) ? Tier::Baseline : Tier::Ion;

      default:
        MOZ_CRASH("Bad mode");
    }
}

static bool
Compile(ModuleGenerator& mg, const ShareableBytes& bytecode, const CompileArgs& args,
        UniqueChars* error, CompileMode compileMode)
{
    MOZ_RELEASE_ASSERT(wasm::HaveSignalHandlers());

    auto env = js::MakeUnique<ModuleEnvironment>();
    if (!env)
        return false;

    Decoder d(bytecode.bytes, error);
    if (!DecodeModuleEnvironment(d, env.get()))
        return false;

    if (!mg.init(Move(env), args, compileMode))
        return false;

    if (!DecodeCodeSection(d, mg))
        return false;

    if (!DecodeModuleTail(d, &mg.mutableEnv()))
        return false;

    MOZ_ASSERT(!*error, "unreported error");
    return true;
}

SharedModule
wasm::CompileInitialTier(const ShareableBytes& bytecode, const CompileArgs& args, UniqueChars* error)
{
    ModuleGenerator mg(error, nullptr);

    CompileMode mode = GetInitialCompileMode(args);
    if (!Compile(mg, bytecode, args, error, mode))
        return nullptr;

    return mg.finishModule(bytecode);
}

bool
wasm::CompileTier2(Module& module, const CompileArgs& args, Atomic<bool>* cancelled)
{
    UniqueChars error;
    ModuleGenerator mg(&error, cancelled);

    if (!Compile(mg, module.bytecode(), args, &error, CompileMode::Tier2))
        return false;

    return mg.finishTier2(module);
}
