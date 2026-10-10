package com.ouassimf.jacket.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.ouassimf.jacket.protocol.Rgb

/**
 * Picks one of the jacket's colours: a hue bar, a white-to-vivid bar and a row of shortcuts.
 *
 * There is no darkness control on purpose: how bright the LEDs are is the brightness slider's job, so every
 * colour made here is at full value.
 */
@Composable
fun ColorControl(
    label: String,
    color: Rgb,
    enabled: Boolean,
    onPreview: (Rgb) -> Unit,
    onCommit: (Rgb) -> Unit,
    modifier: Modifier = Modifier,
) {
    val hsv = remember(color) { FloatArray(3).also { android.graphics.Color.RGBToHSV(color.r, color.g, color.b, it) } }
    // White and greys have no hue of their own: keep the bar where the user left it instead of snapping to red.
    val keptHue = remember { floatArrayOf(hsv[0]) }
    if (hsv[1] > MIN_SATURATION) keptHue[0] = hsv[0]
    val hue = keptHue[0]
    val saturation = hsv[1]

    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Box(
                Modifier
                    .size(22.dp)
                    .background(color.toColor(), RoundedCornerShape(6.dp))
                    .border(1.dp, Neon.Line, RoundedCornerShape(6.dp)),
            )
            ValueRow(label, color.toString())
        }

        DragSlider(
            value = hue,
            // Turning the hue of white would change nothing visible, so it also brings the colour back in.
            onPreview = { keptHue[0] = it; onPreview(vivid(it, saturation.atLeastVisible())) },
            onCommit = { keptHue[0] = it; onCommit(vivid(it, saturation.atLeastVisible())) },
            modifier = Modifier.semantics { contentDescription = "$label hue" },
            valueRange = 0f..360f,
            enabled = enabled,
            track = HueTrack,
        )
        DragSlider(
            value = saturation,
            onPreview = { onPreview(vivid(keptHue[0], it)) },
            onCommit = { onCommit(vivid(keptHue[0], it)) },
            modifier = Modifier.semantics { contentDescription = "$label saturation" },
            enabled = enabled,
            track = Brush.horizontalGradient(listOf(Color.White, Color.hsv(hue, 1f, 1f))),
        )

        Row(Modifier.fillMaxWidth().alpha(if (enabled) 1f else 0.4f), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            for (shortcut in Shortcuts) {
                Box(
                    Modifier
                        .weight(1f)
                        .aspectRatio(1f)
                        .clip(CircleShape)
                        .background(shortcut.toColor())
                        .border(if (shortcut == color) 3.dp else 1.dp, if (shortcut == color) Neon.Text else Neon.Line, CircleShape)
                        .clickable(enabled = enabled, role = Role.Button, onClickLabel = "Use $shortcut") { onCommit(shortcut) },
                )
            }
        }
        if (hsv[2] < 0.99f) {
            Text(
                "This colour is dimmed; changing it here brings it back to full strength.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )
        }
    }
}

fun Rgb.toColor(): Color = Color(red = r, green = g, blue = b)

private fun vivid(hue: Float, saturation: Float): Rgb =
    Rgb(android.graphics.Color.HSVToColor(floatArrayOf(hue.coerceIn(0f, 360f), saturation.coerceIn(0f, 1f), 1f)) and 0xFFFFFF)

private fun Float.atLeastVisible() = if (this > MIN_SATURATION) this else 1f

private const val MIN_SATURATION = 0.03f

private val HueTrack = Brush.horizontalGradient(List(7) { Color.hsv(it * 60f % 360f, 1f, 1f) })

private val Shortcuts = listOf(0x00FFCC, 0xFF00FF, 0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x0000FF, 0xFFFFFF).map(::Rgb)
