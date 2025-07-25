/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_
#define DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_

#include "FirstInitializationAttempts.h"

#include "mozilla/Assertions.h"
#include "mozilla/glean/DomQuotaMetrics.h"
#include "mozilla/TelemetryHistogramEnums.h"
#include "nsError.h"

namespace mozilla::dom::quota {

template <typename Initialization, typename StringGenerator>
void FirstInitializationAttempts<Initialization, StringGenerator>::
    RecordFirstInitializationAttempt(const Initialization aInitialization,
                                     const nsresult aRv) {
  MOZ_ASSERT(!FirstInitializationAttemptRecorded(aInitialization));

  mFirstInitializationAttempts |= aInitialization;

  if constexpr (!std::is_same_v<StringGenerator, Nothing>) {
    glean::dom_quota::first_initialization_attempt
        .Get(StringGenerator::GetString(aInitialization),
             NS_SUCCEEDED(aRv) ? "true"_ns : "false"_ns)
        .Add();
  }
}

}  // namespace mozilla::dom::quota

#endif  // DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_
