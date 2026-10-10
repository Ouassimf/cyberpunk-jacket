package com.ouassimf.jacket.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridItemScope
import androidx.compose.foundation.lazy.grid.LazyGridScope
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.data.Catalog
import com.ouassimf.jacket.data.Change
import com.ouassimf.jacket.data.ConnectionFailure
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.data.SegmentTarget
import com.ouassimf.jacket.protocol.DeviceInfo
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.EffectInfo
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.PaletteInfo
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.Protocol
import java.util.Locale
import kotlin.math.pow
import kotlin.math.roundToInt

private const val ALL_ZONES = -1

@Composable
fun HomeScreen(repository: JacketRepository, gate: BluetoothGate, contentPadding: PaddingValues) {
    val connection by repository.connection.collectAsStateWithLifecycle()
    val state by repository.state.collectAsStateWithLifecycle()
    val catalog by repository.catalog.collectAsStateWithLifecycle()
    val info by repository.info.collectAsStateWithLifecycle()
    val demo by repository.demoMode.collectAsStateWithLifecycle()

    val online = connection == ConnectionStatus.Ready || connection == ConnectionStatus.Syncing
    val current = state
    // The zone the effect controls edit and show; "all" until the user picks one, and again if it disappears.
    var selectedZone by rememberSaveable { mutableIntStateOf(ALL_ZONES) }
    val target = SegmentTarget.of(selectedZone.takeIf { it != ALL_ZONES }, current?.segments?.size ?: 0)
    val config = current?.segments?.getOrNull(target.shown)?.config

    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 104.dp),
        modifier = Modifier.fillMaxSize(),
        contentPadding = contentPadding,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        fullWidth("title") {
            Text("JACKET", style = MaterialTheme.typography.headlineSmall.copy(brush = Neon.Sweep))
        }
        fullWidth("link") {
            LinkCard(
                ui = linkUi(connection, gate.problem.takeUnless { demo || online }, demo, info),
                onAction = { action ->
                    when (action) {
                        LinkAction.Connect -> repository.connect()
                        LinkAction.Disconnect -> repository.disconnect()
                        LinkAction.FixBluetooth -> gate.resolve()
                    }
                },
            )
        }

        if (current == null || config == null) {
            fullWidth("empty") {
                Text(
                    "Nothing to control yet. Connect to the jacket, or switch on Demo mode in Settings to look around.",
                    modifier = Modifier.padding(vertical = 24.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = Neon.TextDim,
                )
            }
            return@LazyVerticalGrid
        }

        fullWidth("power") { PowerPanel(current, online, repository) }

        if (current.segments.size > 1) {
            fullWidth("zones") { ZoneSelector(current.segments.size, target, online) { selectedZone = it ?: ALL_ZONES } }
        }

        fullWidth("effects-label") { SectionLabel("Effect") }
        if (catalog.effects.isEmpty()) {
            fullWidth("effects-loading") {
                Text("Asking the jacket what it can do…", style = MaterialTheme.typography.bodyMedium, color = Neon.TextDim)
            }
        }
        items(catalog.effects, key = { it.id }, contentType = { "effect" }) { effect ->
            EffectTile(
                effect = effect,
                selected = effect.id == config.effect,
                enabled = online,
                onSelect = { repository.commit(Change.SegmentParam(target.edited, Param.Effect(effect.id))) },
            )
        }

        val effect = catalog.effects.firstOrNull { it.id == config.effect }
        if (effect != null) {
            fullWidth("controls") { EffectControls(effect, config, target.edited, catalog, online, repository) }
        }
    }
}

private fun LazyGridScope.fullWidth(key: String, content: @Composable LazyGridItemScope.() -> Unit) =
    item(key = key, span = { GridItemSpan(maxLineSpan) }, contentType = key, content = { content() })

// ---- connection ------------------------------------------------------------------------------------------------

private enum class LinkAction { Connect, Disconnect, FixBluetooth }

private class LinkUi(
    val title: String,
    val detail: String? = null,
    val color: Color = Neon.TextDim,
    val busy: Boolean = false,
    val button: String? = null,
    val action: LinkAction = LinkAction.Connect,
    /** A quiet outlined button for things the user rarely wants, a filled one for the way forward. */
    val quiet: Boolean = false,
)

private fun linkUi(connection: ConnectionStatus, problem: BluetoothProblem?, demo: Boolean, info: DeviceInfo?): LinkUi {
    when (problem) {
        BluetoothProblem.Unavailable -> return LinkUi(
            "No Bluetooth on this device",
            "Switch on Demo mode in Settings to try the app without a jacket.",
            Neon.Warning,
        )
        BluetoothProblem.PermissionNeeded -> return LinkUi(
            "Bluetooth permission needed",
            "Allow \"Nearby devices\" so the app can find and talk to the jacket. Location is never used.",
            Neon.Warning, button = "Allow", action = LinkAction.FixBluetooth,
        )
        BluetoothProblem.PermissionBlocked -> return LinkUi(
            "Bluetooth permission denied",
            "Allow \"Nearby devices\" for Jacket in the system settings.",
            Neon.Warning, button = "Settings", action = LinkAction.FixBluetooth,
        )
        BluetoothProblem.BluetoothOff -> return LinkUi(
            "Bluetooth is off",
            "Switch it on to reach the jacket.",
            Neon.Warning, button = "Turn on", action = LinkAction.FixBluetooth,
        )
        null -> Unit
    }
    val who = if (demo) "demo jacket" else "jacket"
    return when (connection) {
        ConnectionStatus.Disconnected -> LinkUi("Not connected", button = "Connect")
        ConnectionStatus.Scanning -> LinkUi(
            "Searching for the jacket…", "Make sure it is switched on and nearby.",
            Neon.Cyan, busy = true, button = "Stop", action = LinkAction.Disconnect, quiet = true,
        )
        ConnectionStatus.Connecting -> LinkUi(
            "Connecting to the $who…", color = Neon.Cyan, busy = true, button = "Stop", action = LinkAction.Disconnect, quiet = true,
        )
        ConnectionStatus.Syncing -> LinkUi("Connected, reading the $who…", color = Neon.Cyan, busy = true)
        ConnectionStatus.Ready -> LinkUi(
            if (demo) "Demo mode" else "Connected",
            if (demo) "A simulated jacket. Nothing is sent over Bluetooth." else info?.let { "Firmware ${it.version}" },
            Neon.Cyan, button = "Disconnect".takeUnless { demo }, action = LinkAction.Disconnect, quiet = true,
        )
        is ConnectionStatus.Failed -> LinkUi(
            title = when (connection.reason) {
                ConnectionFailure.BluetoothOff -> "Bluetooth is off"
                ConnectionFailure.PermissionMissing -> "Bluetooth permission missing"
                ConnectionFailure.MtuTooLow -> "This phone cannot use the jacket's link"
                ConnectionFailure.UnsupportedProtocol -> "Jacket and app do not match"
                ConnectionFailure.NoReply -> "The jacket does not answer"
            },
            detail = when (connection.reason) {
                ConnectionFailure.MtuTooLow -> "The Bluetooth packet size the phone agreed to is too small for the protocol."
                ConnectionFailure.UnsupportedProtocol -> "The firmware speaks another protocol version. Update the app or the firmware."
                ConnectionFailure.NoReply -> "It connected but stayed silent. Try again, or restart the jacket."
                else -> null
            },
            color = Neon.Danger, button = "Retry",
        )
    }
}

@Composable
private fun LinkCard(ui: LinkUi, onAction: (LinkAction) -> Unit) {
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            if (ui.busy) {
                CircularProgressIndicator(Modifier.size(16.dp), color = ui.color, strokeWidth = 2.dp)
            } else {
                StatusDot(ui.color)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(ui.title, style = MaterialTheme.typography.titleSmall, color = Neon.Text)
                if (ui.detail != null) Text(ui.detail, style = MaterialTheme.typography.bodySmall, color = Neon.TextDim)
            }
            if (ui.button != null) {
                if (ui.quiet) {
                    OutlinedButton(onClick = { onAction(ui.action) }) { Text(ui.button) }
                } else {
                    Button(onClick = { onAction(ui.action) }) { Text(ui.button) }
                }
            }
        }
    }
}

// ---- power and brightness --------------------------------------------------------------------------------------

@Composable
private fun PowerPanel(state: JacketState, enabled: Boolean, repository: JacketRepository) {
    Panel {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(20.dp)) {
            PowerButton(state.power, enabled) { repository.commit(Change.Power(it)) }
            Column(Modifier.weight(1f)) {
                ValueRow("Brightness", percent(state.brightness))
                ByteSlider(
                    value = state.brightness,
                    onPreview = { repository.preview(Change.Brightness(it)) },
                    onCommit = { repository.commit(Change.Brightness(it)) },
                    modifier = Modifier.semantics { contentDescription = "Brightness" },
                    enabled = enabled,
                )
                Text(
                    if (state.power) "The jacket is on" else "The jacket is off",
                    style = MaterialTheme.typography.bodySmall,
                    color = Neon.TextDim,
                )
            }
        }
    }
}

@Composable
private fun PowerButton(on: Boolean, enabled: Boolean, onToggle: (Boolean) -> Unit) {
    val lit = on && enabled
    val glow = if (lit) Neon.Cyan else Color.Transparent
    Box(
        Modifier
            .size(96.dp)
            .alpha(if (enabled) 1f else 0.4f)
            .shadow(if (lit) 18.dp else 0.dp, CircleShape, ambientColor = glow, spotColor = glow)
            .clip(CircleShape)
            .background(if (on) Neon.PanelRaised else Neon.Background)
            .border(3.dp, if (on) Neon.Sweep else SolidColor(Neon.Line), CircleShape)
            .toggleable(value = on, enabled = enabled, role = Role.Switch, onValueChange = onToggle)
            .semantics { contentDescription = "Power" },
        contentAlignment = Alignment.Center,
    ) {
        // The usual power glyph: a ring open at the top with a bar through the gap.
        Canvas(Modifier.size(40.dp)) {
            val stroke = Stroke(width = 4.dp.toPx(), cap = StrokeCap.Round)
            val color = if (on) Neon.Cyan else Neon.TextDim
            val inset = stroke.width / 2
            drawArc(
                color = color,
                startAngle = -60f,
                sweepAngle = 300f,
                useCenter = false,
                topLeft = Offset(inset, inset),
                size = Size(size.width - stroke.width, size.height - stroke.width),
                style = stroke,
            )
            drawLine(color, Offset(center.x, inset), Offset(center.x, center.y), stroke.width, StrokeCap.Round)
        }
    }
}

// ---- zones and effects -------------------------------------------------------------------------------------------

/** Chooses what the effect controls below it address: every zone at once, or one of them. */
@Composable
private fun ZoneSelector(count: Int, target: SegmentTarget, enabled: Boolean, onSelect: (Int?) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionLabel("Zone")
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).selectableGroup(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ChoiceChip("All", target.edited == Protocol.ALL_SEGMENTS, enabled, Neon.Cyan) { onSelect(null) }
            for (index in 0 until count) {
                ChoiceChip("${index + 1}", target.edited == index, enabled, zoneColor(index), Modifier.semantics { contentDescription = "Zone ${index + 1}" }) {
                    onSelect(index)
                }
            }
        }
    }
}

@Composable
private fun EffectTile(effect: EffectInfo, selected: Boolean, enabled: Boolean, onSelect: () -> Unit) {
    val accent = if (selected) Neon.Magenta else Neon.Cyan
    Box(
        Modifier
            .fillMaxWidth()
            .height(68.dp)
            .alpha(if (enabled) 1f else 0.4f)
            .shadow(if (selected) 12.dp else 0.dp, PanelShape, ambientColor = Neon.Magenta, spotColor = Neon.Magenta)
            .clip(PanelShape)
            .background(if (selected) Neon.PanelRaised else Neon.Panel)
            .border(if (selected) 2.dp else 1.dp, accent.copy(alpha = if (selected) 1f else 0.45f), PanelShape)
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onSelect)
            .padding(horizontal = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            effect.name,
            style = MaterialTheme.typography.labelLarge,
            color = if (selected) Neon.Text else Neon.Cyan,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        // What the effect reacts to, when it reacts to anything: live input arrives in a later phase.
        if (effect.params.sound || effect.params.motion) {
            Row(Modifier.align(Alignment.BottomCenter).padding(bottom = 5.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (effect.params.sound) ReactsTo("SOUND")
                if (effect.params.motion) ReactsTo("MOTION")
            }
        }
    }
}

@Composable
private fun ReactsTo(what: String) {
    Text(what, fontSize = 9.sp, lineHeight = 10.sp, fontWeight = FontWeight.Bold, color = Neon.TextDim, maxLines = 1)
}

/** The controls the selected effect says it uses, and only those. */
@Composable
private fun EffectControls(
    effect: EffectInfo,
    config: EffectConfig,
    segment: Int,
    catalog: Catalog,
    enabled: Boolean,
    repository: JacketRepository,
) {
    val params = effect.params
    // Palette 0 is the gradient between the two colours, so an effect that takes its colours from the palette
    // uses both of them while that palette is selected, whatever its own colour bits say (PROTOCOL.md, CATALOG).
    val coloursArePalette = params.palette && config.palette == 0
    val showColor1 = params.color1 || coloursArePalette
    val showColor2 = params.color2 || coloursArePalette
    fun preview(param: Param) = repository.preview(Change.SegmentParam(segment, param))
    fun commit(param: Param) = repository.commit(Change.SegmentParam(segment, param))

    Panel {
        Text(effect.name, style = MaterialTheme.typography.titleMedium, color = Neon.Magenta)

        if (showColor1) {
            ColorControl("Colour 1", config.color1, enabled, { preview(Param.Color1(it)) }, { commit(Param.Color1(it)) })
        }
        if (showColor2) {
            ColorControl("Colour 2", config.color2, enabled, { preview(Param.Color2(it)) }, { commit(Param.Color2(it)) })
        }
        if (params.palette && catalog.palettes.isNotEmpty()) {
            PaletteChooser(catalog.palettes, config.palette, enabled) { commit(Param.Palette(it)) }
        }
        if (params.speed) {
            Column {
                ValueRow("Speed", speedLabel(config.speed))
                ByteSlider(
                    value = config.speed,
                    onPreview = { preview(Param.Speed(it)) },
                    onCommit = { commit(Param.Speed(it)) },
                    modifier = Modifier.semantics { contentDescription = "Speed" },
                    enabled = enabled,
                )
            }
        }
        if (params.intensity) {
            Column {
                ValueRow("Intensity", percent(config.intensity))
                ByteSlider(
                    value = config.intensity,
                    onPreview = { preview(Param.Intensity(it)) },
                    onCommit = { commit(Param.Intensity(it)) },
                    modifier = Modifier.semantics { contentDescription = "Intensity" },
                    enabled = enabled,
                )
            }
        }
        if (!(showColor1 || showColor2 || params.palette || params.speed || params.intensity)) {
            Text("This effect has nothing to adjust.", style = MaterialTheme.typography.bodyMedium, color = Neon.TextDim)
        }
    }
}

@Composable
private fun PaletteChooser(palettes: List<PaletteInfo>, selected: Int, enabled: Boolean, onSelect: (Int) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text("Palette", style = MaterialTheme.typography.labelLarge, color = Neon.Text)
        LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            items(palettes, key = { it.id }) { palette ->
                FilterChip(
                    selected = palette.id == selected,
                    onClick = { onSelect(palette.id) },
                    label = { Text(palette.name) },
                    enabled = enabled,
                    colors = FilterChipDefaults.filterChipColors(
                        labelColor = Neon.Cyan,
                        selectedContainerColor = Neon.Cyan,
                        selectedLabelColor = Neon.Background,
                    ),
                )
            }
        }
    }
}

private fun percent(byte: Int) = "${(byte * 100 / 255f).roundToInt()}%"

/** The document's speed curve: the time multiplier is 2^((speed − 128) / 32). */
private fun speedLabel(speed: Int) = String.format(Locale.ROOT, "×%.2f", 2.0.pow((speed - 128) / 32.0))
