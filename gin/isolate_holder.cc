// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gin/public/isolate_holder.h"

#include <stdlib.h>
#include <string.h>

#include "base/logging.h"
#include "base/rand_util.h"
#include "base/sys_info.h"
#include "gin/array_buffer.h"
#include "gin/function_template.h"
#include "gin/per_isolate_data.h"
#include "gin/public/v8_platform.h"

namespace gin {

namespace {

v8::ArrayBuffer::Allocator* g_array_buffer_allocator = NULL;

bool GenerateEntropy(unsigned char* buffer, size_t amount) {
  base::RandBytes(buffer, amount);
  return true;
}

}  // namespace

IsolateHolder::IsolateHolder() {
  CHECK(g_array_buffer_allocator)
      << "You need to invoke gin::IsolateHolder::Initialize first";
  isolate_ = v8::Isolate::New();
  v8::ResourceConstraints constraints;
  constraints.ConfigureDefaults(base::SysInfo::AmountOfPhysicalMemory(),
                                base::SysInfo::AmountOfVirtualMemory(),
                                base::SysInfo::NumberOfProcessors());
  v8::SetResourceConstraints(isolate_, &constraints);
  isolate_data_.reset(new PerIsolateData(isolate_, g_array_buffer_allocator));
}

IsolateHolder::~IsolateHolder() {
  isolate_data_.reset();
  isolate_->Dispose();
}

// static
void IsolateHolder::Initialize(ScriptMode mode,
                               v8::ArrayBuffer::Allocator* allocator) {
  CHECK(allocator);
  static bool v8_is_initialized = false;
  if (v8_is_initialized)
    return;
  v8::V8::InitializePlatform(V8Platform::Get());
  v8::V8::SetArrayBufferAllocator(allocator);
  g_array_buffer_allocator = allocator;
  if (mode == gin::IsolateHolder::kStrictMode) {
    static const char v8_flags[] = "--use_strict";
    v8::V8::SetFlagsFromString(v8_flags, sizeof(v8_flags) - 1);
  }
  v8::V8::SetEntropySource(&GenerateEntropy);
  v8::V8::Initialize();
  v8_is_initialized = true;
}

}  // namespace gin
