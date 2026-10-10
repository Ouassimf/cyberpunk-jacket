package com.ouassimf.jacket.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/** The palette of the old web page: neon cyan and magenta on near-black. */
object Neon {
    val Cyan = Color(0xFF00FFCC)
    val Magenta = Color(0xFFFF00FF)
    val Background = Color(0xFF0D0D0D)
    val Panel = Color(0xFF141416)
    val PanelRaised = Color(0xFF1C1C20)
    val Line = Color(0xFF263330)
    val Text = Color(0xFFE8FFFA)
    val TextDim = Color(0xFF86A09A)
    val Danger = Color(0xFFFF3B5C)
    val Warning = Color(0xFFFFC400)

    /** The magenta-to-cyan sweep of the old buttons. */
    val Sweep = Brush.linearGradient(listOf(Magenta, Cyan))
}

private val Colors = darkColorScheme(
    primary = Neon.Cyan,
    onPrimary = Neon.Background,
    primaryContainer = Color(0xFF00382D),
    onPrimaryContainer = Neon.Cyan,
    secondary = Neon.Magenta,
    onSecondary = Neon.Background,
    secondaryContainer = Color(0xFF3A003A),
    onSecondaryContainer = Color(0xFFFFB3FF),
    tertiary = Neon.Magenta,
    background = Neon.Background,
    onBackground = Neon.Text,
    surface = Neon.Background,
    onSurface = Neon.Text,
    surfaceVariant = Neon.PanelRaised,
    onSurfaceVariant = Neon.TextDim,
    surfaceContainerLowest = Neon.Background,
    surfaceContainerLow = Neon.Panel,
    surfaceContainer = Neon.Panel,
    surfaceContainerHigh = Neon.PanelRaised,
    surfaceContainerHighest = Neon.PanelRaised,
    outline = Neon.Line,
    outlineVariant = Neon.Line,
    error = Neon.Danger,
    onError = Neon.Background,
)

// The device's own monospace face stands in for the web page's pixel font: no font files, no downloads.
private val Mono = FontFamily.Monospace

private val Type = Typography().run {
    copy(
        headlineSmall = headlineSmall.copy(fontFamily = Mono, fontWeight = FontWeight.Bold, letterSpacing = 4.sp),
        titleMedium = titleMedium.copy(fontFamily = Mono, fontWeight = FontWeight.Bold),
        titleSmall = titleSmall.copy(fontFamily = Mono, fontWeight = FontWeight.Bold),
        labelLarge = labelLarge.copy(fontFamily = Mono, fontWeight = FontWeight.Bold, letterSpacing = 1.sp),
        labelMedium = labelMedium.copy(fontFamily = Mono, letterSpacing = 1.sp),
        labelSmall = labelSmall.copy(fontFamily = Mono, letterSpacing = 1.sp),
    )
}

/** One colour per zone, so a zone looks the same on the strip bars, in lists and on Home. */
private val ZoneColors = listOf(
    Neon.Cyan, Neon.Magenta, Color(0xFFFFD600), Color(0xFF39FF14),
    Color(0xFFFF8A00), Color(0xFF4D8BFF), Color(0xFFFF3B5C), Color(0xFFB388FF),
)

fun zoneColor(index: Int): Color = ZoneColors[index.mod(ZoneColors.size)]

/** Section headings: small, spaced-out capitals. */
val SectionStyle = TextStyle(fontFamily = Mono, fontWeight = FontWeight.Bold, fontSize = 12.sp, letterSpacing = 3.sp)

@Composable
fun JacketTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, typography = Type, content = content)
}
