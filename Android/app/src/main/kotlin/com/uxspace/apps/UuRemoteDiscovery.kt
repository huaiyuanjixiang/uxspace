package com.uxspace.apps

import android.content.Context
import android.util.Log

/**
 * Discovers NetEase UU Remote from the normal Android launcher list.
 *
 * No UU package name is hard-coded: regional/beta builds may use different package ids.
 * We deliberately use the same [InstalledApps] data UxSpace already trusts for its drawer.
 */
object UuRemoteDiscovery {
    private const val TAG = "UxSpace/UURemote"

    data class Match(
        val app: InstalledApp,
        val score: Int,
        val reasons: List<String>,
    )

    /** Best UU Remote candidate, or null when no plausible launcher activity is installed. */
    fun find(context: Context): InstalledApp? =
        rank(InstalledApps.query(context)).firstOrNull()?.app

    /** Best candidate from an already-populated app cache. */
    fun find(apps: List<InstalledApp>): InstalledApp? = rank(apps).firstOrNull()?.app

    /** Rank candidates for diagnostics and future build variants. */
    fun rank(apps: List<InstalledApp>): List<Match> =
        apps.mapNotNull { app ->
            val label = app.label.lowercase()
            val pkg = app.packageName.lowercase()
            val activity = app.activityName.lowercase()
            var score = 0
            val reasons = mutableListOf<String>()

            fun hit(points: Int, reason: String) {
                score += points
                reasons += reason
            }

            if (label.contains("uu") && (label.contains("远程") || label.contains("remote"))) {
                hit(100, "label identifies UU Remote")
            }
            if (label.contains("uu")) hit(15, "label contains UU")
            if (label.contains("远程") || label.contains("remote")) hit(20, "label indicates remote access")
            if (pkg.contains("uu")) hit(10, "package contains uu")
            if (pkg.contains("remote")) hit(10, "package contains remote")
            if (pkg.contains("netease") || pkg.contains("163")) hit(5, "package hints NetEase")
            if (activity.contains("remote")) hit(3, "activity contains remote")

            if (score >= 25) Match(app, score, reasons) else null
        }.sortedByDescending { it.score }

    /** Log all plausible candidates. Useful on vivo/OriginOS variants where package ids differ. */
    fun logCandidates(context: Context) {
        val matches = rank(InstalledApps.query(context))
        if (matches.isEmpty()) {
            Log.w(TAG, "No plausible UU Remote launcher activity found")
            return
        }
        matches.forEachIndexed { index, match ->
            val app = match.app
            Log.i(
                TAG,
                "candidate #$index score=${match.score} label='${app.label}' " +
                    "pkg=${app.packageName} activity=${app.activityName} " +
                    "reasons=${match.reasons.joinToString()}",
            )
        }
    }
}
