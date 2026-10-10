package com.ouassimf.jacket.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

val PanelShape = RoundedCornerShape(10.dp)

/** A bordered block grouping related controls. */
@Composable
fun Panel(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Neon.Panel, PanelShape)
            .border(1.dp, Neon.Line, PanelShape)
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(text.uppercase(), modifier = modifier.padding(top = 8.dp), style = SectionStyle, color = Neon.Cyan)
}

/** A label on the left and the current value on the right, above a control. */
@Composable
fun ValueRow(label: String, value: String, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = Neon.Text)
        Text(value, style = MaterialTheme.typography.labelMedium, color = Neon.TextDim)
    }
}

/**
 * A slider that tells a drag from its end, which is what the repository needs: [onPreview] for every value while
 * the finger is down, [onCommit] once with the last value when it lifts (a tap on the track is both).
 *
 * While dragging it shows its own value, so it stays smooth whatever happens to [value] meanwhile.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DragSlider(
    value: Float,
    onPreview: (Float) -> Unit,
    onCommit: (Float) -> Unit,
    modifier: Modifier = Modifier,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f,
    enabled: Boolean = true,
    track: Brush? = null,
) {
    var dragged by remember { mutableStateOf<Float?>(null) }
    val commit by rememberUpdatedState(onCommit)
    // Leaving the screen mid-drag must still end the gesture, or the repository would wait for it forever.
    DisposableEffect(Unit) {
        onDispose { dragged?.let(commit) }
    }

    Slider(
        value = dragged ?: value,
        onValueChange = {
            dragged = it
            onPreview(it)
        },
        onValueChangeFinished = {
            dragged?.let(commit)
            dragged = null
        },
        modifier = modifier.alpha(if (enabled) 1f else 0.4f),
        enabled = enabled,
        valueRange = valueRange,
        thumb = {
            Box(
                Modifier
                    .size(22.dp)
                    .shadow(8.dp, CircleShape, ambientColor = Neon.Magenta, spotColor = Neon.Magenta)
                    .background(Neon.Magenta, CircleShape),
            )
        },
        track = { state ->
            val span = state.valueRange.endInclusive - state.valueRange.start
            val fraction = if (span > 0f) ((state.value - state.valueRange.start) / span).coerceIn(0f, 1f) else 0f
            Canvas(Modifier.fillMaxWidth().height(8.dp)) {
                val radius = CornerRadius(size.height / 2)
                if (track != null) {
                    drawRoundRect(track, cornerRadius = radius)
                } else {
                    drawRoundRect(SolidColor(Neon.PanelRaised), cornerRadius = radius)
                    drawRoundRect(SolidColor(Neon.Cyan), size = Size(size.width * fraction, size.height), cornerRadius = radius)
                }
            }
        },
    )
}

/** [DragSlider] over a byte-sized protocol value. Previews only when the integer value actually changes. */
@Composable
fun ByteSlider(
    value: Int,
    onPreview: (Int) -> Unit,
    onCommit: (Int) -> Unit,
    modifier: Modifier = Modifier,
    range: IntRange = 0..255,
    enabled: Boolean = true,
) {
    val last = remember { intArrayOf(NOTHING_SENT) }
    DragSlider(
        value = value.toFloat(),
        onPreview = {
            val next = it.roundToInt()
            if (next != last[0]) {
                last[0] = next
                onPreview(next)
            }
        },
        onCommit = {
            last[0] = NOTHING_SENT
            onCommit(it.roundToInt())
        },
        modifier = modifier,
        valueRange = range.first.toFloat()..range.last.toFloat(),
        enabled = enabled,
    )
}

private const val NOTHING_SENT = -1

/** A setting that is on or off: the whole row is the control, the switch shows where it stands. */
@Composable
fun SwitchRow(title: String, detail: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .alpha(if (enabled) 1f else 0.4f)
            .toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = Neon.Text)
            Text(detail, style = MaterialTheme.typography.bodySmall, color = Neon.TextDim)
        }
        // The row is the control; the switch only shows its state.
        Switch(
            checked = checked,
            onCheckedChange = null,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Neon.Background,
                checkedTrackColor = Neon.Cyan,
                uncheckedThumbColor = Neon.TextDim,
                uncheckedTrackColor = Neon.PanelRaised,
                uncheckedBorderColor = Neon.Line,
            ),
        )
    }
}

/**
 * An outlined button in the accent colour, for things to do on the page. Material's own outlined button is
 * grey, which this app keeps for the quiet ways out (stop, disconnect).
 */
@Composable
fun NeonOutlinedButton(onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true, content: @Composable RowScope.() -> Unit) {
    OutlinedButton(
        onClick = onClick,
        modifier = modifier,
        enabled = enabled,
        colors = ButtonDefaults.outlinedButtonColors(contentColor = Neon.Cyan),
        border = BorderStroke(1.dp, Neon.Cyan.copy(alpha = if (enabled) 0.6f else 0.15f)),
        content = content,
    )
}

/** One option of a small either-or row; put the row in a `selectableGroup`. Filled with [accent] when chosen. */
@Composable
fun ChoiceChip(
    label: String,
    selected: Boolean,
    enabled: Boolean,
    accent: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(8.dp)
    Box(
        modifier
            .alpha(if (enabled) 1f else 0.4f)
            .defaultMinSize(minWidth = 52.dp, minHeight = 40.dp)
            .clip(shape)
            .background(if (selected) accent else Neon.Panel)
            .border(1.dp, accent.copy(alpha = if (selected) 1f else 0.6f), shape)
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = MaterialTheme.typography.labelLarge, color = if (selected) Neon.Background else accent, maxLines = 1)
    }
}

/** A small filled dot, used as a status light. */
@Composable
fun StatusDot(color: Color, modifier: Modifier = Modifier) {
    Box(
        modifier
            .size(10.dp)
            .shadow(6.dp, CircleShape, ambientColor = color, spotColor = color)
            .background(color, CircleShape),
    )
}
