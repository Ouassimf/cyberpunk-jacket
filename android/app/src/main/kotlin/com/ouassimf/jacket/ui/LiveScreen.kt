package com.ouassimf.jacket.ui

import android.Manifest
import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.data.Catalog
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.live.LiveController
import com.ouassimf.jacket.live.LiveMeters
import com.ouassimf.jacket.live.LiveNotice
import com.ouassimf.jacket.live.LiveSettings
import com.ouassimf.jacket.live.LiveStatus
import com.ouassimf.jacket.protocol.JacketState
import kotlin.math.roundToInt

/**
 * Live mode: the phone as the jacket's microphone and motion sensor.
 *
 * [startLive] starts it with whatever must surround it; in the app that is the foreground service. Stopping and
 * every setting go straight to [live].
 */
@Composable
fun LiveScreen(repository: JacketRepository, live: LiveController, startLive: (Context) -> Unit, contentPadding: PaddingValues) {
    val connection by repository.connection.collectAsStateWithLifecycle()
    val demo by repository.demoMode.collectAsStateWithLifecycle()
    val state by repository.state.collectAsStateWithLifecycle()
    val catalog by repository.catalog.collectAsStateWithLifecycle()
    val settings by live.settings.collectAsStateWithLifecycle()
    val status by live.status.collectAsStateWithLifecycle()
    val notice by live.notice.collectAsStateWithLifecycle()

    SideEffect { LiveScreenStats.compositions++ }

    val context = LocalContext.current
    val gate = rememberLiveGate()
    val clock = rememberMeterClock(live)
    var askedForNotifications by rememberSaveable { mutableStateOf(false) }

    // Starts with what is allowed; sound without the microphone permission is just not listened to.
    fun launch() {
        val now = live.settings.value
        if (now.motion || (now.sound && gate.microphone == Permit.Granted)) startLive(context)
    }

    fun start() {
        val wanted = buildList {
            if (settings.sound && gate.microphone == Permit.Needed) add(Manifest.permission.RECORD_AUDIO)
            if (gate.notifications == Permit.Needed && !askedForNotifications) add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Manifest.permission.POST_NOTIFICATIONS in wanted) askedForNotifications = true
        if (wanted.isEmpty()) launch() else gate.request(wanted, ::launch)
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("LIVE", style = MaterialTheme.typography.headlineSmall.copy(brush = Neon.Sweep))

        StatusCard(
            ui = statusUi(status, connection, demo, settings, state?.liveInput == true),
            notes = buildList {
                when (notice) {
                    LiveNotice.IdleTimeout -> add("Live mode stopped by itself: the jacket was away for ${LiveController.IDLE_TIMEOUT.inWholeMinutes} minutes.")
                    LiveNotice.MicrophoneUnavailable -> add("The microphone could not be opened. Another app may be using it.")
                    LiveNotice.StartRefused -> add("Android did not let live mode start. Try again with the app open.")
                    null -> Unit
                }
                if (status != LiveStatus.Off && nothingReacts(state, catalog)) {
                    add("No zone is playing a reactive effect. Pick one marked SOUND or MOTION on Home to see the jacket react.")
                }
            },
            onStart = ::start,
            onStop = { live.stop() },
        )

        SectionLabel("Sound")
        Panel {
            SwitchRow(
                title = "Sound",
                detail = "The microphone: how loud it is, bass, mid and treble, and the beat.",
                checked = settings.sound,
                enabled = true,
                onChange = { on -> live.update { it.copy(sound = on) } },
            )
            if (settings.sound && gate.microphone != Permit.Granted) {
                PermissionRow(
                    title = if (gate.microphone == Permit.Blocked) "Microphone permission denied" else "Microphone permission needed",
                    detail = if (gate.microphone == Permit.Blocked) {
                        "Allow the microphone for Jacket in the system settings, or switch Sound off."
                    } else {
                        "Allow the microphone so the phone can hear the music, or switch Sound off."
                    },
                    permit = gate.microphone,
                    // Granted while live mode runs: have the service look again, so it starts listening.
                    onAllow = { gate.request(listOf(Manifest.permission.RECORD_AUDIO)) { if (live.running) launch() } },
                    onSettings = gate::openSettings,
                )
            }
            Meter("Level", clock, Neon.Cyan, MeterClock.LEVEL)
            Meter("Bass", clock, Neon.Magenta, MeterClock.BASS)
            Meter("Mid", clock, MidColor, MeterClock.MID)
            Meter("Treble", clock, TrebleColor, MeterClock.TREBLE)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                BeatLight(clock)
                Tempo(clock)
            }
            Text(
                "Sound is analysed on the phone as it comes in. It is never recorded and never leaves the phone: only these few numbers go to the jacket.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )
        }

        SectionLabel("Motion")
        Panel {
            SwitchRow(
                title = "Motion",
                detail = "The phone's accelerometer: how much you move, and which way the phone is tilted.",
                checked = settings.motion,
                enabled = true,
                onChange = { on -> live.update { it.copy(motion = on) } },
            )
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Meter("Motion", clock, Neon.Cyan, MeterClock.MOTION)
                    Text("The dot shows the tilt: centred when the phone lies flat.", style = MaterialTheme.typography.bodySmall, color = Neon.TextDim)
                }
                TiltPad(clock)
            }
        }

        SectionLabel("Tuning")
        Panel {
            ValueRow("Sensitivity", "${settings.sensitivity}%")
            DragSlider(
                value = settings.sensitivity.toFloat(),
                onPreview = { value -> live.update { it.copy(sensitivity = value.roundToInt()) } },
                onCommit = { value -> live.update { it.copy(sensitivity = value.roundToInt()) } },
                modifier = Modifier.semantics { contentDescription = "Sensitivity" },
                valueRange = LiveSettings.SENSITIVITY_RANGE.first.toFloat()..LiveSettings.SENSITIVITY_RANGE.last.toFloat(),
            )
            Text(
                "The meters find their own range within a few seconds. Raise this if quiet music or small movements do too little.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )

            ValueRow("Beat timing", offsetText(settings.latencyOffsetMs))
            DragSlider(
                value = settings.latencyOffsetMs.toFloat(),
                onPreview = { value -> live.update { it.copy(latencyOffsetMs = snapOffset(value)) } },
                onCommit = { value -> live.update { it.copy(latencyOffsetMs = snapOffset(value)) } },
                modifier = Modifier.semantics { contentDescription = "Beat timing" },
                valueRange = LiveSettings.OFFSET_RANGE_MS.first.toFloat()..LiveSettings.OFFSET_RANGE_MS.last.toFloat(),
            )
            Text(
                "With music on and a beat effect playing, slide until the flash lands on the beat: right if the jacket is late, left if it is early.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )
        }

        if (gate.notifications != Permit.Granted) {
            Panel {
                PermissionRow(
                    title = "Notifications are off",
                    detail = "Live mode keeps running with the screen off. Allow notifications to see that it does, and to stop it without opening the app.",
                    permit = gate.notifications,
                    onAllow = {
                        askedForNotifications = true
                        gate.request(listOf(Manifest.permission.POST_NOTIFICATIONS))
                    },
                    onSettings = gate::openSettings,
                )
            }
        }
    }
}

// ---- status ----------------------------------------------------------------------------------------------------

private class StatusUi(
    val title: String,
    val detail: String,
    val color: Color,
    val busy: Boolean = false,
    val running: Boolean = false,
    val canStart: Boolean = false,
)

private fun statusUi(status: LiveStatus, connection: ConnectionStatus, demo: Boolean, settings: LiveSettings, confirmed: Boolean): StatusUi {
    val who = if (demo) "demo jacket" else "jacket"
    val background = "Keeps going with the screen off."
    return when (status) {
        LiveStatus.Streaming -> StatusUi(
            title = "Streaming to the $who",
            detail = when {
                demo -> "Demo mode: the frames go to the simulated jacket, nothing is sent over Bluetooth."
                confirmed -> "The jacket is receiving, ${LiveController.FRAMES_PER_SECOND} times a second. $background"
                else -> "Sending ${LiveController.FRAMES_PER_SECOND} times a second. $background"
            },
            color = Neon.Cyan,
            running = true,
        )
        LiveStatus.Waiting -> StatusUi(
            title = "Waiting for the $who…",
            detail = "Still listening. Streaming resumes when the $who is back; after ${LiveController.IDLE_TIMEOUT.inWholeMinutes} minutes without it, live mode stops by itself.",
            color = Neon.Warning,
            busy = true,
            running = true,
        )
        LiveStatus.Off -> when {
            connection != ConnectionStatus.Ready -> StatusUi(
                title = if (demo) "Demo jacket not ready" else "Jacket not connected",
                detail = if (demo) "Live mode starts once the demo jacket is connected." else "Connect to the jacket on Home first. Live mode sends to it.",
                color = Neon.Warning,
            )
            !settings.anySource -> StatusUi("Live mode is off", "Switch on Sound or Motion below to have something to send.", Neon.TextDim)
            else -> StatusUi(
                title = "Live mode is off",
                detail = "The phone becomes the $who's microphone and motion sensor, and reactive effects follow the music and your movement.",
                color = Neon.TextDim,
                canStart = true,
            )
        }
    }
}

/** True when the jacket's catalogue is known and no zone plays an effect that declares it reacts to sound or motion. */
private fun nothingReacts(state: JacketState?, catalog: Catalog): Boolean {
    if (state == null || !catalog.complete) return false
    return state.segments.none { segment ->
        val params = catalog.effects.firstOrNull { it.id == segment.config.effect }?.params
        params != null && (params.sound || params.motion)
    }
}

@Composable
private fun StatusCard(ui: StatusUi, notes: List<String>, onStart: () -> Unit, onStop: () -> Unit) {
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            if (ui.busy) {
                CircularProgressIndicator(Modifier.size(16.dp), color = ui.color, strokeWidth = 2.dp)
            } else {
                StatusDot(ui.color)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(ui.title, style = MaterialTheme.typography.titleSmall, color = Neon.Text)
                Text(ui.detail, style = MaterialTheme.typography.bodySmall, color = Neon.TextDim)
            }
            if (ui.running) {
                OutlinedButton(onClick = onStop) { Text("Stop") }
            } else {
                Button(onClick = onStart, enabled = ui.canStart) { Text("Start") }
            }
        }
        for (note in notes) Text(note, style = MaterialTheme.typography.bodySmall, color = Neon.Warning)
    }
}

@Composable
private fun PermissionRow(title: String, detail: String, permit: Permit, onAllow: () -> Unit, onSettings: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        StatusDot(Neon.Warning)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = Neon.Text)
            Text(detail, style = MaterialTheme.typography.bodySmall, color = Neon.TextDim)
        }
        if (permit == Permit.Blocked) {
            NeonOutlinedButton(onClick = onSettings) { Text("Settings") }
        } else {
            NeonOutlinedButton(onClick = onAllow) { Text("Allow") }
        }
    }
}

// ---- meters ----------------------------------------------------------------------------------------------------

/**
 * Drives the meters without recomposing anything. The controller refills its [LiveMeters] thirty times a second
 * and says so; that bumps [tick], which only the meters' draw code reads, so Compose redraws those few canvases
 * and nothing else. The tempo is the exception: it is text, so it is state, but one that changes when the
 * rounded BPM does, not every frame.
 *
 * What is sent is deliberately raw; the jacket smooths it. The bars are smoothed here the same way (sound about
 * 25 ms up and 180 ms down, motion 60 and 400), so they move like the light does rather than flicker.
 */
@Stable
private class MeterClock(val meters: LiveMeters) {
    val tick = mutableIntStateOf(0)
    val bpm = mutableIntStateOf(0)

    /** The bars as drawn, 0 to 255: level, bass, mid, treble, motion. */
    val bars = FloatArray(5)

    fun advance() {
        follow(LEVEL, meters.level, meters.audioValid, SOUND_UP, SOUND_DOWN)
        follow(BASS, meters.bass, meters.audioValid, SOUND_UP, SOUND_DOWN)
        follow(MID, meters.mid, meters.audioValid, SOUND_UP, SOUND_DOWN)
        follow(TREBLE, meters.treble, meters.audioValid, SOUND_UP, SOUND_DOWN)
        follow(MOTION, meters.motion, meters.motionValid, MOTION_UP, MOTION_DOWN)
        tick.intValue++
        val shown = if (meters.beatValid) (meters.bpmX10 + 5) / 10 else 0
        if (bpm.intValue != shown) bpm.intValue = shown
    }

    /** A source that is not being sent has nothing to fade: its bar is empty at once (live mode stopped, say). */
    private fun follow(bar: Int, target: Int, valid: Boolean, up: Float, down: Float) {
        bars[bar] = if (valid) bars[bar] + (target - bars[bar]) * (if (target > bars[bar]) up else down) else 0f
    }

    companion object {
        const val LEVEL = 0
        const val BASS = 1
        const val MID = 2
        const val TREBLE = 3
        const val MOTION = 4

        // Per frame of the 30 Hz stream: 1 - exp(-33 ms / time constant).
        private const val SOUND_UP = 0.74f
        private const val SOUND_DOWN = 0.17f
        private const val MOTION_UP = 0.43f
        private const val MOTION_DOWN = 0.08f
    }
}

@Composable
private fun rememberMeterClock(live: LiveController): MeterClock {
    val clock = remember(live) { MeterClock(live.meters) }
    DisposableEffect(clock) {
        clock.advance()
        live.onMeters = clock::advance
        onDispose { live.onMeters = null }
    }
    return clock
}

/**
 * How often the Live screen or one of its meters was composed. The meters must only ever redraw; the screen
 * test watches this number stand still while they move.
 */
internal object LiveScreenStats {
    var compositions = 0
}

private val MidColor = Color(0xFFFFD600)
private val TrebleColor = Color(0xFF4D8BFF)

/** A horizontal bar for one of the [MeterClock.bars]. */
@Composable
private fun Meter(label: String, clock: MeterClock, color: Color, bar: Int) {
    SideEffect { LiveScreenStats.compositions++ }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(label.uppercase(), modifier = Modifier.width(64.dp), style = MaterialTheme.typography.labelMedium, color = Neon.TextDim, maxLines = 1)
        Canvas(
            Modifier
                .weight(1f)
                .height(10.dp)
                .semantics { contentDescription = "$label meter" },
        ) {
            clock.tick.intValue // read here, in the draw phase: a new frame redraws this canvas and nothing more
            val radius = CornerRadius(size.height / 2)
            drawRoundRect(Neon.PanelRaised, cornerRadius = radius)
            val fraction = (clock.bars[bar] / 255f).coerceIn(0f, 1f)
            if (fraction > 0f) drawRoundRect(color, size = Size(maxOf(size.height, size.width * fraction), size.height), cornerRadius = radius)
        }
    }
}

/** Flashes on every beat the phone hears and fades until the next; a dim ring when there is no beat. */
@Composable
private fun BeatLight(clock: MeterClock) {
    SideEffect { LiveScreenStats.compositions++ }
    Canvas(Modifier.size(36.dp).semantics { contentDescription = "Beat" }) {
        clock.tick.intValue
        val meters = clock.meters
        val radius = size.minDimension / 2
        if (meters.beatValid) {
            val glow = (1f - meters.heardPhase).coerceIn(0f, 1f).let { it * it * it }
            drawCircle(Neon.Magenta.copy(alpha = 0.15f + 0.85f * glow), radius * (0.6f + 0.4f * glow))
        }
        drawCircle(if (meters.beatValid) Neon.Magenta else Neon.Line, radius - 1.dp.toPx(), style = Stroke(1.dp.toPx()))
    }
}

/** The tempo in words. Its own composable so that a new tempo recomposes these two lines and nothing around them. */
@Composable
private fun Tempo(clock: MeterClock) {
    val bpm = clock.bpm.intValue
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(
            if (bpm > 0) "$bpm BPM" else "– BPM",
            modifier = Modifier.semantics { contentDescription = "Tempo" },
            style = MaterialTheme.typography.titleMedium,
            color = if (bpm > 0) Neon.Text else Neon.TextDim,
        )
        Text(
            if (bpm > 0) "The light pulses on the beat the phone hears." else "No steady beat heard.",
            style = MaterialTheme.typography.bodySmall,
            color = Neon.TextDim,
        )
    }
}

/** A square with a dot at the phone's tilt: right when the right edge is up, up when the top edge is. */
@Composable
private fun TiltPad(clock: MeterClock) {
    SideEffect { LiveScreenStats.compositions++ }
    Canvas(Modifier.size(72.dp).semantics { contentDescription = "Tilt" }) {
        clock.tick.intValue
        val meters = clock.meters
        val line = 1.dp.toPx()
        drawRoundRect(Neon.PanelRaised, cornerRadius = CornerRadius(8.dp.toPx()))
        drawRoundRect(Neon.Line, cornerRadius = CornerRadius(8.dp.toPx()), style = Stroke(line))
        drawLine(Neon.Line, Offset(center.x, 6.dp.toPx()), Offset(center.x, size.height - 6.dp.toPx()), line)
        drawLine(Neon.Line, Offset(6.dp.toPx(), center.y), Offset(size.width - 6.dp.toPx(), center.y), line)
        val reach = size.minDimension / 2 - 10.dp.toPx()
        val dot = Offset(center.x + reach * meters.tiltX / 127f, center.y - reach * meters.tiltY / 127f)
        drawCircle(if (meters.motionValid) Neon.Cyan else Neon.TextDim.copy(alpha = 0.5f), 5.dp.toPx(), dot)
    }
}

// ---- tuning ----------------------------------------------------------------------------------------------------

private const val OFFSET_STEP_MS = 5

private fun snapOffset(value: Float): Int =
    ((value / OFFSET_STEP_MS).roundToInt() * OFFSET_STEP_MS).coerceIn(LiveSettings.OFFSET_RANGE_MS)

private fun offsetText(ms: Int): String = when {
    ms > 0 -> "+$ms ms"
    ms < 0 -> "−${-ms} ms"
    else -> "0 ms"
}
