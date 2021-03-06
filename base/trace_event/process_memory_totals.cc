// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/trace_event/process_memory_totals.h"

#include "base/trace_event/trace_event_argument.h"

namespace base {
namespace trace_event {

void ProcessMemoryTotals::AsValueInto(TracedValue* value) const {
  value->SetDouble("resident_set_bytes", resident_set_bytes_);
}

}  // namespace trace_event
}  // namespace base
