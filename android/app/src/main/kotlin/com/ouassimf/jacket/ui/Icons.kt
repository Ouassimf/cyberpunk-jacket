package com.ouassimf.jacket.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.unit.dp

/** The handful of icons the app needs, as path data: cheaper than a whole icon library for a few tabs. */
object AppIcons {
    val Home: ImageVector by lazy { icon("Home", "M10,20v-6h4v6h5v-8h3L12,3 2,12h3v8z") }

    val Settings: ImageVector by lazy {
        icon(
            "Settings",
            "M3,17v2h6v-2H3zM3,5v2h10V5H3zM13,21v-2h8v-2h-8v-2h-2v6h2zM7,9v2H3v2h4v2h2V9H7zM21,13v-2H11v2h10zM15,9h2V7h4V5h-4V3h-2v6z",
        )
    }

    /** Two strips cut into zones. */
    val Zones: ImageVector by lazy { icon("Zones", "M3,5h7v5H3z M12,5h9v5h-9z M3,14h11v5H3z M16,14h5v5h-5z") }

    /** Level bars. */
    val Live: ImageVector by lazy { icon("Live", "M3,10h2v4H3z M7,6h2v12H7z M11,3h2v18h-2z M15,8h2v8h-2z M19,11h2v2h-2z") }

    val Presets: ImageVector by lazy { icon("Presets", "M17,3H7c-1.1,0 -2,0.9 -2,2v16l7,-3 7,3V5c0,-1.1 -0.9,-2 -2,-2z") }

    private fun icon(name: String, path: String): ImageVector =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
            .addPath(pathData = addPathNodes(path), fill = SolidColor(Color.Black))
            .build()
}
