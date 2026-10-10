package com.ouassimf.jacket.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.data.LayoutProblem
import com.ouassimf.jacket.data.Zones
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry
import kotlin.math.abs
import kotlin.math.roundToInt

/** How long the jacket keeps a range lit: a moment after a drag, longer when asked for by button. */
private const val FOLLOW_SECONDS = 3
private const val IDENTIFY_SECONDS = 6

/**
 * Where the zones are. Nobody has written down how the strips sit on the jacket, so this screen is the way to
 * find out: drag the ends of a zone and the jacket lights the range as it moves.
 *
 * Edits go into a draft that only this screen sees. Nothing but the "show me" light reaches the jacket until
 * Apply, which sends the whole layout once.
 */
@Composable
fun ZonesScreen(repository: JacketRepository, contentPadding: PaddingValues) {
    val connection by repository.connection.collectAsStateWithLifecycle()
    val state by repository.state.collectAsStateWithLifecycle()
    val info by repository.info.collectAsStateWithLifecycle()
    val online = connection == ConnectionStatus.Ready || connection == ConnectionStatus.Syncing
    val current = state
    val strips = info?.leds

    var draft by rememberSaveable(stateSaver = SegmentsSaver) { mutableStateOf<List<Segment>?>(null) }
    var selectedIndex by rememberSaveable { mutableIntStateOf(0) }
    var follow by rememberSaveable { mutableStateOf(true) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("ZONES", style = MaterialTheme.typography.headlineSmall.copy(brush = Neon.Sweep))

        if (current == null || strips.isNullOrEmpty()) {
            Text(
                "Connect to the jacket to lay out its zones.",
                modifier = Modifier.padding(vertical = 24.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = Neon.TextDim,
            )
            return@Column
        }

        val maxSegments = info?.maxSegments ?: 1
        val segments = draft ?: current.segments
        val selected = selectedIndex.coerceIn(segments.indices)
        val zone = segments[selected].geometry
        val problems = Zones.problems(segments.map { it.geometry }, strips, maxSegments)
        val changed = draft != null && draft != current.segments

        fun light(geometry: SegmentGeometry, seconds: Int, dragging: Boolean = false) =
            repository.identify(geometry.stripMask, geometry.start, geometry.length, seconds, dragging)

        /** Every edit lands here. With [dragging] the finger is still down, so the light just follows. */
        fun edit(next: List<Segment>, select: Int = selected, dragging: Boolean = false) {
            draft = next
            selectedIndex = select.coerceIn(next.indices)
            if (follow) light(next[selectedIndex].geometry, FOLLOW_SECONDS, dragging)
        }

        Text(
            "Cut the strips into zones here, then give each zone its own effect on Home. Where zones overlap, the higher number shows.",
            style = MaterialTheme.typography.bodySmall,
            color = Neon.TextDim,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NeonOutlinedButton(onClick = { edit(Zones.mirrored(segments, strips), select = 0) }, enabled = online) { Text("Mirrored") }
            NeonOutlinedButton(onClick = { edit(Zones.perStrip(segments, strips), select = 0) }, enabled = online && strips.size > 1) {
                Text(if (strips.size == 2) "Left + Right" else "One per strip")
            }
        }

        Panel {
            strips.forEachIndexed { strip, length ->
                StripBar(
                    name = stripName(strip, strips.size),
                    strip = strip,
                    length = length,
                    segments = segments,
                    selected = selected,
                    enabled = online,
                    onSelect = { selectedIndex = it },
                    onMoveStart = { edit(Zones.moveStart(segments, selected, it), dragging = true) },
                    onMoveEnd = { edit(Zones.moveEnd(segments, selected, it, strips), dragging = true) },
                    // The drag is over: one last, acknowledged "show me" so the light does not die mid-look.
                    onDragEnd = { if (follow) light((draft ?: segments)[selected].geometry, FOLLOW_SECONDS) },
                )
            }
        }

        if (changed) {
            Panel {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        if (problems.isEmpty()) "Not on the jacket yet." else problemText(problems.first()),
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall,
                        color = if (problems.isEmpty()) Neon.TextDim else Neon.Danger,
                    )
                    TextButton(onClick = { draft = null }) { Text("Discard") }
                    Button(
                        onClick = {
                            repository.applyLayout(segments)
                            draft = null
                        },
                        enabled = online && problems.isEmpty(),
                    ) { Text("Apply") }
                }
            }
        }

        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            // Eight chips do not fit a phone: they scroll, and the add button stays put at the end.
            Row(
                Modifier.weight(1f).horizontalScroll(rememberScrollState()).selectableGroup(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                for (index in segments.indices) {
                    ChoiceChip("${index + 1}", index == selected, true, zoneColor(index), Modifier.semantics { contentDescription = "Zone ${index + 1}" }) {
                        selectedIndex = index
                    }
                }
            }
            NeonOutlinedButton(
                onClick = { Zones.add(segments, selected, strips, maxSegments).let { edit(it, select = it.lastIndex) } },
                enabled = online && segments.size < maxSegments,
                modifier = Modifier.semantics { contentDescription = "Add a zone" },
            ) { Text("+") }
        }

        Panel {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Zone ${selected + 1}", Modifier.weight(1f), style = MaterialTheme.typography.titleMedium, color = zoneColor(selected))
                Text(
                    "${zone.start}–${zone.end - 1} · ${zone.length} ${if (zone.length == 1) "LED" else "LEDs"}",
                    modifier = Modifier.semantics { contentDescription = "Zone range" },
                    style = MaterialTheme.typography.labelLarge,
                    color = Neon.Text,
                )
            }
            Stepper("First LED", zone.start, online) { edit(Zones.moveStart(segments, selected, zone.start + it)) }
            Stepper("Last LED", zone.end - 1, online) { edit(Zones.moveEnd(segments, selected, zone.end + it, strips)) }

            if (strips.size == 2) {
                Row(Modifier.selectableGroup(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Strip", Modifier.weight(1f), style = MaterialTheme.typography.labelLarge, color = Neon.Text)
                    for ((label, mask) in listOf("Left" to 1, "Right" to 2, "Both" to 3)) {
                        ChoiceChip(label, zone.stripMask == mask, online, Neon.Cyan) { edit(Zones.setStrips(segments, selected, mask, strips)) }
                    }
                }
            }
            SwitchRow(
                title = "Reversed",
                detail = "The effect runs from the last LED of the zone to the first.",
                checked = zone.reversed,
                enabled = online,
                onChange = { edit(Zones.flip(segments, selected)) },
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                NeonOutlinedButton(onClick = { light(zone, IDENTIFY_SECONDS) }, enabled = online) { Text("Identify") }
                if (segments.size > 1) {
                    TextButton(
                        onClick = { edit(Zones.remove(segments, selected), select = (selected - 1).coerceAtLeast(0)) },
                        enabled = online,
                        colors = ButtonDefaults.textButtonColors(contentColor = Neon.Danger),
                    ) { Text("Remove") }
                }
            }
        }

        Panel {
            SwitchRow(
                title = "Light up while editing",
                detail = "The jacket shows the zone in dim white for a few seconds whenever you move it.",
                checked = follow,
                enabled = true,
                onChange = { follow = it },
            )
        }
    }
}

private fun stripName(strip: Int, count: Int) = when {
    count == 2 && strip == 0 -> "Left"
    count == 2 && strip == 1 -> "Right"
    else -> "Strip ${strip + 1}"
}

private fun problemText(problem: LayoutProblem): String {
    val zone = problem.segment?.let { "Zone ${it + 1}" } ?: "The layout"
    return when (problem.kind) {
        LayoutProblem.Kind.NoSegments -> "There must be at least one zone."
        LayoutProblem.Kind.TooManySegments -> "The jacket cannot hold that many zones."
        LayoutProblem.Kind.NoStrip, LayoutProblem.Kind.UnknownStrip -> "$zone is on no strip the jacket has."
        LayoutProblem.Kind.Empty -> "$zone has no LEDs."
        LayoutProblem.Kind.OutsideStrip -> "$zone runs past the end of its strip."
        LayoutProblem.Kind.UnknownFlags -> "$zone has settings this app does not know."
    }
}

/** A number with a step down and a step up: for the last LED or two, where a finger on the bar is too coarse. */
@Composable
private fun Stepper(label: String, value: Int, enabled: Boolean, onStep: (Int) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.labelLarge, color = Neon.Text)
        NeonOutlinedButton(onClick = { onStep(-1) }, enabled = enabled, modifier = Modifier.semantics { contentDescription = "$label down" }) { Text("−") }
        Text("$value", Modifier.width(44.dp), style = MaterialTheme.typography.titleMedium, color = Neon.Text, textAlign = TextAlign.Center)
        NeonOutlinedButton(onClick = { onStep(1) }, enabled = enabled, modifier = Modifier.semantics { contentDescription = "$label up" }) { Text("+") }
    }
}

/**
 * One strip as a bar, with every zone that sits on it. Tap a zone to select it; drag either end of the selected
 * zone to resize it, snapped to whole LEDs. Higher-numbered zones are drawn on top, as the jacket renders them.
 */
@Composable
private fun StripBar(
    name: String,
    strip: Int,
    length: Int,
    segments: List<Segment>,
    selected: Int,
    enabled: Boolean,
    onSelect: (Int) -> Unit,
    onMoveStart: (Int) -> Unit,
    onMoveEnd: (Int) -> Unit,
    onDragEnd: () -> Unit,
) {
    val bit = 1 shl strip
    val measurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold, fontSize = 13.sp, color = Neon.Background)
    // The gesture below outlives recompositions (it must: every drag step recomposes), so it reads these fresh.
    val latestSegments by rememberUpdatedState(segments)
    val latestSelected by rememberUpdatedState(selected)
    val moveStart by rememberUpdatedState(onMoveStart)
    val moveEnd by rememberUpdatedState(onMoveEnd)
    val dragEnd by rememberUpdatedState(onDragEnd)
    val select by rememberUpdatedState(onSelect)

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        ValueRow(name, "$length LEDs")
        Canvas(
            Modifier
                .fillMaxWidth()
                .height(52.dp)
                .alpha(if (enabled) 1f else 0.4f)
                .semantics { contentDescription = "$name strip" }
                .pointerInput(length, bit, enabled) {
                    if (!enabled) return@pointerInput
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        val perLed = size.width.toFloat() / length
                        val zone = latestSegments.getOrNull(latestSelected)?.geometry?.takeIf { it.stripMask and bit != 0 }
                        val toStart = zone?.let { abs(down.position.x - it.start * perLed) } ?: Float.MAX_VALUE
                        val toEnd = zone?.let { abs(down.position.x - it.end * perLed) } ?: Float.MAX_VALUE
                        if (minOf(toStart, toEnd) <= HANDLE_REACH.toPx()) {
                            val start = toStart < toEnd
                            down.consume()
                            drag(down.id) { change ->
                                val led = (change.position.x / perLed).roundToInt()
                                if (start) moveStart(led) else moveEnd(led)
                                change.consume()
                            }
                            dragEnd()
                        } else if (waitForUpOrCancellation() != null) {
                            val led = (down.position.x / perLed).toInt()
                            val hit = latestSegments.indexOfLast { it.geometry.stripMask and bit != 0 && led in it.geometry.start until it.geometry.end }
                            if (hit >= 0) select(hit)
                        }
                    }
                },
        ) {
            val perLed = size.width / length
            val corner = CornerRadius(6.dp.toPx())
            drawRoundRect(Neon.PanelRaised, cornerRadius = corner)
            for (tick in 1..3) {
                val x = size.width * tick / 4
                drawLine(Neon.Line, Offset(x, 0f), Offset(x, size.height), 1.dp.toPx())
            }
            segments.forEachIndexed { index, segment ->
                val g = segment.geometry
                if (g.stripMask and bit == 0) return@forEachIndexed
                val isSelected = index == selected
                val inset = if (isSelected) 0f else 7.dp.toPx()
                val left = g.start * perLed
                val width = g.length * perLed
                drawRoundRect(
                    color = zoneColor(index).copy(alpha = if (isSelected) 1f else 0.7f),
                    topLeft = Offset(left, inset),
                    size = Size(width, size.height - 2 * inset),
                    cornerRadius = corner,
                )
                if (isSelected) {
                    drawRoundRect(Color.White, Offset(left, 0f), Size(width, size.height), corner, style = Stroke(2.dp.toPx()))
                    for (x in listOf(left, left + width)) {
                        drawRoundRect(
                            color = Color.White,
                            topLeft = Offset((x - 3.dp.toPx()).coerceIn(0f, size.width - 6.dp.toPx()), size.height * 0.2f),
                            size = Size(6.dp.toPx(), size.height * 0.6f),
                            cornerRadius = CornerRadius(3.dp.toPx()),
                        )
                    }
                }
            }
            // Each zone's number, with a mark on the side its effect runs towards, in the middle of the longest
            // stretch of it that no higher zone covers: where it can be read, and where the zone really shows.
            segments.forEachIndexed { index, segment ->
                val g = segment.geometry
                if (g.stripMask and bit == 0) return@forEachIndexed
                val (from, until) = visibleStretch(segments, index, bit)
                val label = measurer.measure(if (g.reversed) "‹ ${index + 1}" else "${index + 1} ›", labelStyle)
                val room = (until - from) * perLed
                if (label.size.width + 6.dp.toPx() <= room) {
                    drawText(label, topLeft = Offset(from * perLed + (room - label.size.width) / 2, (size.height - label.size.height) / 2))
                }
            }
        }
    }
}

/** The longest run of LEDs of zone [index], on the strip of [bit], that no higher-numbered zone covers: `from` to `until`. */
private fun visibleStretch(segments: List<Segment>, index: Int, bit: Int): Pair<Int, Int> {
    val g = segments[index].geometry
    val above = segments.drop(index + 1).map { it.geometry }.filter { it.stripMask and bit != 0 }
    var best = 0 to 0
    var runStart = g.start
    for (led in g.start..g.end) {
        if (led == g.end || above.any { led in it.start until it.end }) {
            if (led - runStart > best.second - best.first) best = runStart to led
            runStart = led + 1
        }
    }
    return best
}

/** How far from an end of the selected zone a touch still grabs it. */
private val HANDLE_REACH = 28.dp

/** Keeps a draft layout across rotation, as ten numbers per segment (each colour packed into one). */
private val SegmentsSaver = Saver<List<Segment>?, IntArray>(
    save = { segments ->
        segments.orEmpty().flatMap { (g, c) ->
            listOf(g.stripMask, g.start, g.length, g.flags, c.effect, c.palette, c.speed, c.intensity, c.color1.packed, c.color2.packed)
        }.toIntArray()
    },
    restore = { numbers ->
        numbers.toList().chunked(10).map { n ->
            Segment(SegmentGeometry(n[0], n[1], n[2], n[3]), EffectConfig(n[4], n[5], n[6], n[7], Rgb(n[8]), Rgb(n[9])))
        }.ifEmpty { null }
    },
)
