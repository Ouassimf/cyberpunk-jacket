package com.ouassimf.jacket.link

import com.ouassimf.jacket.protocol.CatalogEntry
import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.DeviceInfo
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.EffectInfo
import com.ouassimf.jacket.protocol.ErrorCode
import com.ouassimf.jacket.protocol.ErrorReply
import com.ouassimf.jacket.protocol.FrameException
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.Notification
import com.ouassimf.jacket.protocol.PaletteInfo
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.ParamFlags
import com.ouassimf.jacket.protocol.PresetInfo
import com.ouassimf.jacket.protocol.PresetList
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry
import com.ouassimf.jacket.protocol.utf8Size
import com.ouassimf.jacket.protocol.with
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.IOException

/**
 * A jacket that lives inside the app and speaks the real frames, following PROTOCOL.md rule for rule: the v1
 * catalogue, validation with its precedence and error codes, the `rev` and `preset` rules, presets, segments,
 * STATE coalesced to about ten per second. Demo mode and the tests run against it, so they exercise the real
 * codec and the behaviour the firmware is held to.
 *
 * It is a freshly booted jacket with empty storage: power on, one segment over both strips. Its presets live
 * in memory and are gone with the process.
 *
 * Not thread-safe: use it from [scope]'s (single-threaded) dispatcher only.
 *
 * @param maxPageEntries caps the entries per CATALOG frame, for tests that want paging; the document's rule (as
 *   many as fit in a frame) applies on top.
 * @param notifyIntervalMs minimum time between two STATE notifications.
 */
class FakeJacket(
    private val scope: CoroutineScope,
    private val maxPageEntries: Int = Int.MAX_VALUE,
    private val notifyIntervalMs: Long = 100,
    private val connectDelayMs: Long = 350,
) : JacketLink {

    private val _status = MutableStateFlow<LinkStatus>(LinkStatus.Disconnected)
    override val status: StateFlow<LinkStatus> = _status.asStateFlow()

    private val _frames = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    override val frames: SharedFlow<ByteArray> = _frames.asSharedFlow()

    private val stateChanged = Channel<Unit>(Channel.CONFLATED)
    private var session: Job? = null
    private var liveTimeout: Job? = null

    private var state = JacketState(
        proto = Protocol.VERSION,
        rev = 0,
        power = true,
        brightness = 128,
        budgetMa = Protocol.BUDGET_DEFAULT_MA,
        flags = 0,
        preset = Protocol.NO_PRESET,
        segments = listOf(
            Segment(
                SegmentGeometry(stripMask = ALL_STRIPS, start = 0, length = STRIP_LEDS, flags = 0),
                // What the firmware plays out of the box: Cylon in cyan and magenta.
                EffectConfig(effect = 6, palette = 0, speed = 128, intensity = 128, color1 = Rgb(0x00FFCC), color2 = Rgb(0xFF00FF)),
            ),
        ),
    )
    private val presets = sortedMapOf<Int, Preset>()

    private class Preset(val name: String, val brightness: Int, val segments: List<Segment>)

    /** Test hook: while true the storage is broken, so saving or deleting a preset answers ERROR 5. */
    var storageFails = false

    /** The range last lit by an accepted IDENTIFY, null once cancelled. (The real overlay also ends by itself.) */
    var identified: Command.Identify? = null
        private set

    // ---- JacketLink -------------------------------------------------------------------------------------------

    override fun connect() {
        if (session?.isActive == true) return
        session = scope.launch { run() }
    }

    override fun disconnect() {
        session?.cancel()
        session = null
        _status.value = LinkStatus.Disconnected
    }

    override suspend fun send(frame: ByteArray, withResponse: Boolean) {
        if (_status.value != LinkStatus.Connected) throw IOException("fake jacket is not connected")
        try {
            handle(Codec.decodeCommand(frame))
        } catch (e: FrameException) {
            // A rejected command changes nothing and is answered with ERROR.
            notify(ErrorReply(e.type, e.code))
        }
    }

    override suspend fun sendLive(frame: ByteArray) {
        if (_status.value != LinkStatus.Connected) throw IOException("fake jacket is not connected")
        if (frame.size < Protocol.LIVE_FRAME_SIZE) return // dropped silently
        setFlag(JacketState.FLAG_LIVE, true)
        liveTimeout?.cancel()
        liveTimeout = scope.launch {
            delay(Protocol.LIVE_TIMEOUT_MS)
            setFlag(JacketState.FLAG_LIVE, false)
        }
    }

    override fun setHighPriority(enabled: Boolean) = Unit

    override fun close() = disconnect()

    /** Loses the connection the way a jacket walking out of range does; the link then reconnects by itself. */
    fun simulateDrop() {
        if (session?.isActive != true) return
        session?.cancel()
        session = scope.launch { run() }
    }

    private suspend fun run() {
        _status.value = LinkStatus.Connecting
        delay(connectDelayMs)
        stateChanged.tryReceive() // The read below already carries whatever changed while nobody was connected.
        // What the real link does on connect: subscribe, then read the characteristic, which holds the latest STATE.
        notify(state)
        _status.value = LinkStatus.Connected
        while (true) {
            stateChanged.receive()
            notify(state)
            delay(notifyIntervalMs)
        }
    }

    // ---- the jacket -------------------------------------------------------------------------------------------

    /** [command] is already well-formed (the codec saw to that); what is left is what only the jacket can judge. */
    private fun handle(command: Command) {
        when (command) {
            is Command.SetPower -> accept(state.copy(power = command.on))
            is Command.SetBrightness -> accept(state.copy(brightness = command.value))
            is Command.SetEffect -> {
                checkSegment(command, command.segment)
                if (!knowsEffect(command.config.effect) || !knowsPalette(command.config.palette)) outOfRange(command)
                accept(state.copy(segments = mapSegments(command.segment) { command.config }))
            }
            is Command.SetParam -> {
                checkSegment(command, command.segment)
                val known = when (val param = command.param) {
                    is Param.Effect -> knowsEffect(param.value)
                    is Param.Palette -> knowsPalette(param.value)
                    else -> true
                }
                if (!known) outOfRange(command)
                accept(state.copy(segments = mapSegments(command.segment) { it.with(command.param) }))
            }
            is Command.SetSegments -> {
                if (!command.segments.all(::isValid)) outOfRange(command)
                val first = state.segments[0].config
                accept(
                    state.copy(
                        segments = command.segments.mapIndexed { i, geometry ->
                            Segment(geometry, state.segments.getOrNull(i)?.config ?: first)
                        },
                    ),
                )
            }
            is Command.Identify -> {
                // Not part of STATE: no rev, no notification. Length 0 or seconds 0 cancels, with nothing to check.
                val cancel = command.length == 0 || command.seconds == 0
                if (!cancel && !isValid(SegmentGeometry(command.stripMask, command.start, command.length, 0))) outOfRange(command)
                identified = command.takeUnless { cancel }
            }
            is Command.PresetSave -> {
                checkSlot(command, command.slot)
                if (storageFails) storageFailure(command)
                presets[command.slot] = Preset(command.name, state.brightness, state.segments)
                accept(state, preset = command.slot)
                notifyPresets()
            }
            is Command.PresetLoad -> {
                checkSlot(command, command.slot)
                val preset = presets[command.slot] ?: notFound(command)
                accept(state.copy(brightness = preset.brightness, segments = preset.segments), preset = command.slot)
            }
            is Command.PresetDelete -> {
                checkSlot(command, command.slot)
                if (command.slot !in presets) notFound(command)
                if (storageFails) storageFailure(command)
                presets.remove(command.slot)
                accept(state, preset = if (state.preset == command.slot) Protocol.NO_PRESET else state.preset)
                notifyPresets()
            }
            Command.GetPresets -> notifyPresets()
            Command.GetInfo -> notify(INFO)
            is Command.GetCatalog -> {
                val all: List<CatalogEntry> = when (command.kind) {
                    CatalogKind.EFFECTS -> EFFECTS
                    CatalogKind.PALETTES -> PALETTES
                    else -> outOfRange(command)
                }
                notify(CatalogPage(command.kind, command.start, all.size, page(all, command.start)))
            }
            is Command.SetBudget ->
                accept(state.copy(budgetMa = command.milliamps.coerceIn(Protocol.BUDGET_MIN_MA, Protocol.BUDGET_MAX_MA)))
            is Command.SetDevMode -> accept(state.withFlag(JacketState.FLAG_DEV_MODE, command.on))
            // The demo jacket has no other firmware to go back to.
            is Command.BootOtherSlot ->
                if (command.confirm != Protocol.BOOT_OTHER_SLOT_CONFIRM) outOfRange(command) else notFound(command)
        }
    }

    /**
     * Every accepted command that can change STATE ends here, and the state rules of the document with it:
     * `rev` moves even when the value was already set, and the preset marker survives only if neither brightness
     * nor any segment changed, unless this command is the save or load that sets it ([preset]).
     */
    private fun accept(next: JacketState, preset: Int? = null) {
        val sceneChanged = next.brightness != state.brightness || next.segments != state.segments
        state = next.copy(
            rev = (state.rev + 1) and 0xFFFF,
            preset = preset ?: if (sceneChanged) Protocol.NO_PRESET else state.preset,
        )
        stateChanged.trySend(Unit)
    }

    /** The WiFi and live-input flags are not commands: `rev` moves only when the flag really changes. */
    private fun setFlag(flag: Int, on: Boolean) {
        val next = state.withFlag(flag, on)
        if (next.flags == state.flags) return
        state = next.copy(rev = (state.rev + 1) and 0xFFFF)
        stateChanged.trySend(Unit)
    }

    private fun JacketState.withFlag(flag: Int, on: Boolean) = copy(flags = if (on) flags or flag else flags and flag.inv())

    private inline fun mapSegments(segment: Int, transform: (EffectConfig) -> EffectConfig): List<Segment> =
        state.segments.mapIndexed { i, s ->
            if (segment == Protocol.ALL_SEGMENTS || segment == i) s.copy(config = transform(s.config)) else s
        }

    /** As many entries as fit in a frame, from [start]; none when [start] is at or past the end. */
    private fun page(all: List<CatalogEntry>, start: Int): List<CatalogEntry> {
        val page = ArrayList<CatalogEntry>()
        var size = CATALOG_HEADER_BYTES
        for (entry in all.drop(start)) {
            val entrySize = (if (entry is EffectInfo) 3 else 2) + entry.name.utf8Size
            if (size + entrySize > Protocol.MAX_FRAME || page.size >= maxPageEntries) break
            page += entry
            size += entrySize
        }
        return page
    }

    private fun notifyPresets() = notify(PresetList(presets.map { (slot, preset) -> PresetInfo(slot, preset.name) }))

    private fun notify(notification: Notification) {
        _frames.tryEmit(Codec.encode(notification))
    }

    // ---- validation (code 3 unless said otherwise) --------------------------------------------------------------

    private fun knowsEffect(id: Int) = EFFECTS.any { it.id == id }

    private fun knowsPalette(id: Int) = PALETTES.any { it.id == id }

    private fun isValid(g: SegmentGeometry) =
        g.stripMask != 0 && g.stripMask and ALL_STRIPS.inv() == 0 &&
            g.flags and Protocol.GEOMETRY_REVERSED.inv() == 0 &&
            g.length in 1..STRIP_LEDS && g.start + g.length <= STRIP_LEDS

    private fun checkSegment(command: Command, segment: Int) {
        if (segment != Protocol.ALL_SEGMENTS && segment !in state.segments.indices) outOfRange(command)
    }

    private fun checkSlot(command: Command, slot: Int) {
        if (slot >= Protocol.PRESET_SLOTS) outOfRange(command)
    }

    private fun outOfRange(command: Command): Nothing =
        throw FrameException(command.opcode, ErrorCode.OUT_OF_RANGE, "value out of range in $command")

    private fun notFound(command: Command): Nothing =
        throw FrameException(command.opcode, ErrorCode.NOT_FOUND, "empty slot for $command")

    private fun storageFailure(command: Command): Nothing =
        throw FrameException(command.opcode, ErrorCode.STORAGE_FAILURE, "storage failed for $command")

    companion object {
        private const val STRIP_LEDS = Protocol.MAX_STRIP_LENGTH
        private const val ALL_STRIPS = Protocol.STRIP_LEFT or Protocol.STRIP_RIGHT
        private const val CATALOG_HEADER_BYTES = 5

        /** The v1 catalogue, ids, names and flags as published in PROTOCOL.md. */
        val EFFECTS: List<EffectInfo> = listOf(
            EffectInfo(0, ParamFlags(0x01), "Solid"),
            EffectInfo(1, ParamFlags(0x1D), "Comet"),
            EffectInfo(2, ParamFlags(0x18), "Rainbow"),
            EffectInfo(3, ParamFlags(0x1C), "Matrix"),
            EffectInfo(4, ParamFlags(0x19), "Breathe"),
            EffectInfo(5, ParamFlags(0x19), "Heartbeat"),
            EffectInfo(6, ParamFlags(0x1C), "Cylon"),
            EffectInfo(7, ParamFlags(0x3B), "Beat Pulse"),
            EffectInfo(8, ParamFlags(0x3C), "VU Meter"),
            EffectInfo(9, ParamFlags(0x5C), "Sparkle"),
        )

        val PALETTES: List<PaletteInfo> = listOf(
            PaletteInfo(0, "Colours"),
            PaletteInfo(1, "Rainbow"),
            PaletteInfo(2, "Matrix"),
            PaletteInfo(3, "Cyberpunk"),
            PaletteInfo(4, "Fire"),
            PaletteInfo(5, "Ocean"),
        )

        val INFO = DeviceInfo(
            proto = Protocol.VERSION,
            fwMajor = 2,
            fwMinor = 0,
            fwPatch = 0,
            leds = listOf(STRIP_LEDS, STRIP_LEDS),
            maxSegments = Protocol.MAX_SEGMENTS,
            presetSlots = Protocol.PRESET_SLOTS,
            effectCount = EFFECTS.size,
            paletteCount = PALETTES.size,
            build = "demo",
        )
    }
}
