/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.lib.crash

import android.content.Intent
import android.os.Bundle
import androidx.annotation.StringDef
import mozilla.components.concept.base.crash.Breadcrumb
import mozilla.components.support.utils.ext.getParcelableArrayListCompat
import mozilla.components.support.utils.ext.getSerializableCompat
import java.io.Serializable
import java.util.UUID

// Intent extra used to store crash data under when passing crashes in Intent objects
private const val INTENT_CRASH = "mozilla.components.lib.crash.CRASH"

// Uncaught exception crash intent extras
private const val INTENT_EXCEPTION = "exception"

// Breadcrumbs intent extras
private const val INTENT_BREADCRUMBS = "breadcrumbs"

// Crash timestamp intent extras
private const val INTENT_CRASH_TIMESTAMP = "crashTimestamp"

// Crash runtime tag extras
private const val INTENT_RUNTIME_TAGS = "runtimeTags"

// Native code crash intent extras (Mirroring GeckoView values)
private const val INTENT_UUID = "uuid"
private const val INTENT_MINIDUMP_PATH = "minidumpPath"
private const val INTENT_EXTRAS_PATH = "extrasPath"
private const val INTENT_PROCESS_VISIBILITY = "processVisibility"
private const val INTENT_PROCESS_TYPE = "processType"
private const val INTENT_REMOTE_TYPE = "remoteType"

/**
 * Crash types that are handled by this library.
 */
sealed class Crash {
    /**
     * Unique ID identifying this crash.
     */
    abstract val uuid: String

    /**
     * Runtime tags that should be attached to any report associated with this crash.
     */
    abstract val runtimeTags: Map<String, String>

    /**
     * Breadcrumbs associated with the crash to send with the crash report
     */
    abstract val breadcrumbs: ArrayList<Breadcrumb>

    /**
     * Timestamp time of when the crash happened
     */
    abstract val timestamp: Long

    /**
     * A crash caused by an uncaught exception.
     *
     * @property timestamp Time of when the crash happened.
     * @property throwable The [Throwable] that caused the crash.
     * @property breadcrumbs List of breadcrumbs to send with the crash report.
     * @property runtimeTags Runtime tags that should be attached to any report associated with this crash.
     * @property uuid Unique ID identifying this crash.
     */
    data class UncaughtExceptionCrash(
        override val timestamp: Long,
        val throwable: Throwable,
        override val breadcrumbs: ArrayList<Breadcrumb>,
        override val runtimeTags: Map<String, String> = emptyMap(),
        override val uuid: String = UUID.randomUUID().toString(),
    ) : Crash() {
        override fun toBundle() = Bundle().apply {
            putString(INTENT_UUID, uuid)
            putSerializable(INTENT_EXCEPTION, throwable as Serializable)
            putLong(INTENT_CRASH_TIMESTAMP, timestamp)
            putParcelableArrayList(INTENT_BREADCRUMBS, breadcrumbs)
            putSerializable(INTENT_RUNTIME_TAGS, HashMap(runtimeTags))
        }

        companion object {
            @Suppress("UNCHECKED_CAST", "DEPRECATION")
            internal fun fromBundle(bundle: Bundle) = UncaughtExceptionCrash(
                uuid = bundle.getString(INTENT_UUID) as String,
                throwable = bundle.getSerializableCompat(
                    INTENT_EXCEPTION,
                    Throwable::class.java,
                ) as Throwable,
                breadcrumbs = bundle.getParcelableArrayListCompat(
                    INTENT_BREADCRUMBS,
                    Breadcrumb::class.java,
                )
                    ?: arrayListOf(),
                timestamp = bundle.getLong(INTENT_CRASH_TIMESTAMP, System.currentTimeMillis()),
                runtimeTags = bundle.getSerializable(INTENT_RUNTIME_TAGS) as? HashMap<String, String>
                    ?: hashMapOf(),
            )
        }
    }

    /**
     * A crash that happened in native code.
     *
     * @property timestamp Time of when the crash happened.
     * @property minidumpPath Path to a Breakpad minidump file containing information about the crash.
     * @property extrasPath Path to a file containing extra metadata about the crash. The file contains key-value pairs
     *                      in the form `Key=Value`. Be aware, it may contain sensitive data such as the URI that was
     *                      loaded at the time of the crash.
     * @property processVisibility The type of process the crash occurred in. Affects whether or not the crash is fatal
     *                       or whether the application can recover from it.
     * @property processType The name of the type of process the crash occurred in. This matches the process types
     *                       reported by gecko.
     * @property breadcrumbs List of breadcrumbs to send with the crash report.
     * @property remoteType The type of child process (when available).
     * @property runtimeTags Runtime tags that should be attached to any report associated with this crash.
     * @property uuid Unique ID identifying this crash.
     */
    data class NativeCodeCrash(
        override val timestamp: Long,
        val minidumpPath: String?,
        val extrasPath: String?,
        @param:ProcessVisibility val processVisibility: String?,
        val processType: String?,
        override val breadcrumbs: ArrayList<Breadcrumb>,
        val remoteType: String?,
        override val runtimeTags: Map<String, String> = emptyMap(),
        override val uuid: String = UUID.randomUUID().toString(),
    ) : Crash() {
        override fun toBundle() = Bundle().apply {
            putString(INTENT_UUID, uuid)
            putString(INTENT_MINIDUMP_PATH, minidumpPath)
            putString(INTENT_EXTRAS_PATH, extrasPath)
            putString(INTENT_PROCESS_VISIBILITY, processVisibility)
            putString(INTENT_PROCESS_TYPE, processType)
            putLong(INTENT_CRASH_TIMESTAMP, timestamp)
            putParcelableArrayList(INTENT_BREADCRUMBS, breadcrumbs)
            putString(INTENT_REMOTE_TYPE, remoteType)
            putSerializable(INTENT_RUNTIME_TAGS, HashMap(runtimeTags))
        }

        /**
         * Whether the crash was fatal or not: If true, the main application process was affected by
         * the crash. If false, only an internal process used by Gecko has crashed and the application
         * may be able to recover.
         */
        val isFatal: Boolean
            get() = processVisibility == PROCESS_VISIBILITY_MAIN

        companion object {
            /**
             * Indicates a crash occurred in the main process and is therefore fatal.
             */
            const val PROCESS_VISIBILITY_MAIN = "MAIN"

            /**
             * Indicates a crash occurred in a foreground child process. The application may be
             * able to recover from this crash, but it was likely noticeable to the user.
             */
            const val PROCESS_VISIBILITY_FOREGROUND_CHILD = "FOREGROUND_CHILD"

            /**
             * Indicates a crash occurred in a background child process. This should have been
             * recovered from automatically, and will have had minimal impact to the user, if any.
             */
            const val PROCESS_VISIBILITY_BACKGROUND_CHILD = "BACKGROUND_CHILD"

            /**
             * Process visibility strings.
             */
            @StringDef(
                PROCESS_VISIBILITY_MAIN,
                PROCESS_VISIBILITY_FOREGROUND_CHILD,
                PROCESS_VISIBILITY_BACKGROUND_CHILD,
            )
            @Retention(AnnotationRetention.SOURCE)
            annotation class ProcessVisibility

            @Suppress("UNCHECKED_CAST", "DEPRECATION")
            internal fun fromBundle(bundle: Bundle) = NativeCodeCrash(
                uuid = bundle.getString(INTENT_UUID) ?: UUID.randomUUID().toString(),
                minidumpPath = bundle.getString(INTENT_MINIDUMP_PATH, null),
                extrasPath = bundle.getString(INTENT_EXTRAS_PATH, null),
                processVisibility = bundle.getString(INTENT_PROCESS_VISIBILITY, PROCESS_VISIBILITY_MAIN),
                processType = bundle.getString(INTENT_PROCESS_TYPE, "main"),
                breadcrumbs = bundle.getParcelableArrayListCompat(
                    INTENT_BREADCRUMBS,
                    Breadcrumb::class.java,
                )
                    ?: arrayListOf(),
                remoteType = bundle.getString(INTENT_REMOTE_TYPE, null),
                timestamp = bundle.getLong(INTENT_CRASH_TIMESTAMP, System.currentTimeMillis()),
                runtimeTags = bundle.getSerializable(INTENT_RUNTIME_TAGS) as? HashMap<String, String>
                    ?: hashMapOf(),
            )
        }
    }

    internal abstract fun toBundle(): Bundle

    internal fun fillIn(intent: Intent) {
        intent.putExtra(INTENT_CRASH, toBundle())
    }

    /**
     * Returns a new crash with the passed in tags added
     */
    fun withTags(tags: Map<String, String>): Crash {
        return when (this) {
            is NativeCodeCrash -> this.copy(
                runtimeTags = runtimeTags.toMutableMap().apply { putAll(tags) },
            )

            is UncaughtExceptionCrash -> this.copy(
                runtimeTags = runtimeTags.toMutableMap().apply { putAll(tags) },
            )
        }
    }

    companion object {
        fun fromIntent(intent: Intent): Crash {
            val bundle = intent.getBundleExtra(INTENT_CRASH)!!

            return if (bundle.containsKey(INTENT_MINIDUMP_PATH)) {
                NativeCodeCrash.fromBundle(bundle)
            } else {
                UncaughtExceptionCrash.fromBundle(bundle)
            }
        }

        fun isCrashIntent(intent: Intent) = intent.extras?.containsKey(INTENT_CRASH) ?: false
    }
}

/**
 * Interface used when implemented to provide tags to attach to crashes at runtime
 */
interface RuntimeTagProvider {

    /**
     * When invoked, should return relevant runtime tags
     *
     * @return relevant runtime tags
     */
    operator fun invoke(): Map<String, String>
}
