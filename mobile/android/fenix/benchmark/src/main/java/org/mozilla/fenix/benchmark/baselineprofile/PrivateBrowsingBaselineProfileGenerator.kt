/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.benchmark.baselineprofile

import android.content.Intent
import android.net.Uri
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.benchmark.macro.junit4.BaselineProfileRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.benchmark.utils.TARGET_PACKAGE
import org.mozilla.fenix.benchmark.utils.closeTab
import org.mozilla.fenix.benchmark.utils.dismissWallpaperOnboarding
import org.mozilla.fenix.benchmark.utils.isWallpaperOnboardingShown
import org.mozilla.fenix.benchmark.utils.loadSite
import org.mozilla.fenix.benchmark.utils.openNewPrivateTabOnTabsTray
import org.mozilla.fenix.benchmark.utils.openTabsTray

/**
 * This test class generates a baseline profile on a critical user journey, that loads a website from the
 * awesome bar in private browsing mode, for the target package.
 *
 * Refer to the [baseline profile documentation](https://d.android.com/topic/performance/baselineprofiles)
 * for more information.
 *
 * Make sure `autosignReleaseWithDebugKey=true` is present in local.properties.
 *
 * Generate the baseline profile using this gradle task:
 * ```
 *  ./gradlew :benchmark:pixel6Api34BenchmarkAndroidTest -P android.testInstrumentationRunnerArguments.annotation=org.mozilla.fenix.benchmark.baselineprofile -P benchmarkTest -P disableOptimization
 * ```
 *
 * Check [documentation](https://d.android.com/topic/performance/benchmarking/macrobenchmark-instrumentation-args)
 * for more information about available instrumentation arguments.
 *
 * Then, copy the profiles to app/src/benchmark/baselineProfiles to verify the improvements by running
 * the [org.mozilla.fenix.benchmark.BaselineProfilesPrivateBrowsingBenchmark] benchmark.
 * Notice that when we run the benchmark, we run the benchmark variant and not the nightly so
 * copying the profiles here is important.
 * These shouldn't be pushed to version control.
 *
 * When using this class to generate a baseline profile, only API 33+ or rooted API 28+ are supported.
 */
@RequiresApi(Build.VERSION_CODES.P)
@RunWith(AndroidJUnit4::class)
@BaselineProfileGenerator
class PrivateBrowsingBaselineProfileGenerator {

    @get:Rule
    val rule = BaselineProfileRule()

    @Test
    fun generateBaselineProfile() {
        rule.collect(
            packageName = TARGET_PACKAGE,
        ) {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse("fenix-nightly://home"))

            startActivityAndWait(intent = intent)

            if (device.isWallpaperOnboardingShown()) {
                device.dismissWallpaperOnboarding()
            }

            device.openTabsTray(packageName = packageName)
            device.openNewPrivateTabOnTabsTray()
            device.loadSite(packageName = packageName, url = "example.com")

            device.openTabsTray(packageName = packageName)
            device.closeTab(siteName = "Example Domain", siteUrl = "http://example.com")
        }
    }
}
