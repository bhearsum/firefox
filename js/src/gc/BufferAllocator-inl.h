/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_BufferAllocator_inl_h
#define gc_BufferAllocator_inl_h

#include "gc/BufferAllocator.h"

#include "mozilla/Atomics.h"
#include "mozilla/MathAlgorithms.h"

#include "ds/SlimLinkedList.h"
#include "gc/BufferAllocatorInternals.h"
#include "js/HeapAPI.h"

#include "gc/Allocator-inl.h"

namespace js::gc {

static constexpr size_t SmallAllocGranularityShift =
    BufferAllocator::MinSmallAllocShift;
static constexpr size_t MediumAllocGranularityShift =
    BufferAllocator::MinMediumAllocShift;

static constexpr size_t SmallAllocGranularity = 1 << SmallAllocGranularityShift;
static constexpr size_t MediumAllocGranularity = 1
                                                 << MediumAllocGranularityShift;

static constexpr size_t MinSmallAllocSize =
    1 << BufferAllocator::MinSmallAllocShift;
static constexpr size_t MinMediumAllocSize =
    1 << BufferAllocator::MinMediumAllocShift;
static constexpr size_t MinLargeAllocSize =
    1 << BufferAllocator::MinLargeAllocShift;

static constexpr size_t MinAllocSize = MinSmallAllocSize;

static constexpr size_t MaxSmallAllocSize =
    MinMediumAllocSize - SmallAllocGranularity;
static constexpr size_t MaxMediumAllocSize =
    MinLargeAllocSize - MediumAllocGranularity;
static constexpr size_t MaxAlignedAllocSize = MinLargeAllocSize / 4;

// Size classes map to power of two sizes. The full range contains two
// consecutive sub-ranges [MinSmallAllocClass, MaxSmallAllocClass] and
// [MinMediumAllocClass, MaxMediumAllocClass]. MaxSmallAllocClass and
// MinMediumAllocClass are consecutive but both map to the same size, which is
// MinMediumAllocSize.
static constexpr size_t MinSmallAllocClass = 0;
static constexpr size_t MaxSmallAllocClass =
    BufferAllocator::SmallSizeClasses - 1;
static constexpr size_t MinMediumAllocClass = MaxSmallAllocClass + 1;
static constexpr size_t MaxMediumAllocClass =
    MinMediumAllocClass + BufferAllocator::MediumSizeClasses - 1;
static_assert(MaxMediumAllocClass == BufferAllocator::AllocSizeClasses - 1);

/* static */
inline bool BufferAllocator::IsSmallAllocSize(size_t bytes) {
  return bytes <= MaxSmallAllocSize;
}

/* static */
inline bool BufferAllocator::IsLargeAllocSize(size_t bytes) {
  return bytes > MaxMediumAllocSize;
}

/* static */
inline size_t BufferAllocator::GetGoodAllocSize(size_t requiredBytes) {
  requiredBytes = std::max(requiredBytes, MinAllocSize);

  if (IsLargeAllocSize(requiredBytes)) {
    return RoundUp(requiredBytes, ChunkSize);
  }

  if (IsSmallAllocSize(requiredBytes)) {
    return RoundUp(requiredBytes, SmallAllocGranularity);
  }

  return RoundUp(requiredBytes, MediumAllocGranularity);
}

/* static */
size_t BufferAllocator::GetGoodPower2AllocSize(size_t requiredBytes) {
  requiredBytes = std::max(requiredBytes, MinAllocSize);

  return mozilla::RoundUpPow2(requiredBytes);
}

/* static */
size_t BufferAllocator::GetGoodElementCount(size_t requiredElements,
                                            size_t elementSize) {
  size_t requiredBytes = requiredElements * elementSize;
  size_t goodSize = GetGoodAllocSize(requiredBytes);
  return goodSize / elementSize;
}

/* static */
size_t BufferAllocator::GetGoodPower2ElementCount(size_t requiredElements,
                                                  size_t elementSize) {
  size_t requiredBytes = requiredElements * elementSize;
  size_t goodSize = GetGoodPower2AllocSize(requiredBytes);
  return goodSize / elementSize;
}

inline size_t GetGoodAllocSize(size_t requiredBytes) {
  return BufferAllocator::GetGoodAllocSize(requiredBytes);
}

inline size_t GetGoodElementCount(size_t requiredCount, size_t elementSize) {
  return BufferAllocator::GetGoodElementCount(requiredCount, elementSize);
}

inline size_t GetGoodPower2AllocSize(size_t requiredBytes) {
  return BufferAllocator::GetGoodPower2AllocSize(requiredBytes);
}

inline size_t GetGoodPower2ElementCount(size_t requiredCount,
                                        size_t elementSize) {
  return BufferAllocator::GetGoodPower2ElementCount(requiredCount, elementSize);
}

inline void* AllocBuffer(JS::Zone* zone, size_t bytes, bool nurseryOwned) {
  if (js::oom::ShouldFailWithOOM()) {
    return nullptr;
  }

  return zone->bufferAllocator.alloc(bytes, nurseryOwned);
}

inline void* AllocBufferInGC(JS::Zone* zone, size_t bytes, bool nurseryOwned) {
  return zone->bufferAllocator.allocInGC(bytes, nurseryOwned);
}

inline void* ReallocBuffer(JS::Zone* zone, void* alloc, size_t oldBytes,
                           size_t newBytes, bool nurseryOwned) {
  if (js::oom::ShouldFailWithOOM()) {
    return nullptr;
  }

  return zone->bufferAllocator.realloc(alloc, oldBytes, newBytes, nurseryOwned);
}

inline void FreeBuffer(JS::Zone* zone, void* alloc) {
  return zone->bufferAllocator.free(alloc);
}

inline bool IsBufferAlloc(void* alloc) {
  return BufferAllocator::IsBufferAlloc(alloc);
}

inline size_t GetAllocSize(JS::Zone* zone, void* alloc) {
  return zone->bufferAllocator.getAllocSize(alloc);
}

inline bool IsNurseryOwned(JS::Zone* zone, void* alloc) {
  return zone->bufferAllocator.isNurseryOwned(alloc);
}

inline bool IsBufferAllocMarkedBlack(JS::Zone* zone, void* alloc) {
  return zone->bufferAllocator.isMarkedBlack(alloc);
}

inline void TraceBufferEdgeInternal(JSTracer* trc, Cell* owner, void** bufferp,
                                    const char* name) {
  owner->zoneFromAnyThread()->bufferAllocator.traceEdge(trc, owner, bufferp,
                                                        name);
}

inline void MarkTenuredBuffer(JS::Zone* zone, void* alloc) {
  zone->bufferAllocator.markTenuredAlloc(alloc);
}

}  // namespace js::gc

#endif  // gc_BufferAllocator_inl_h
