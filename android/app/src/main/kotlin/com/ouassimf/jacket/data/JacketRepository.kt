package com.ouassimf.jacket.data

import com.ouassimf.jacket.link.JacketLink
import com.ouassimf.jacket.link.LinkFailure
import com.ouassimf.jacket.link.LinkStatus
import com.ouassimf.jacket.protocol.CatalogEntry
import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.DeviceInfo
import com.ouassimf.jacket.protocol.EffectInfo
import com.ouassimf.jacket.protocol.ErrorReply
import com.ouassimf.jacket.protocol.FrameException
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.LiveFrame
import com.ouassimf.jacket.protocol.Notification
import com.ouassimf.jacket.protocol.Opcode
import com.ouassimf.jacket.protocol.PaletteInfo
import com.ouassimf.jacket.protocol.PresetInfo
import com.ouassimf.jacket.protocol.PresetList
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.UnsupportedProtocolException
import com.ouassimf.jacket.protocol.utf8Size
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.flow.onSubscription
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.io.IOException
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds
import kotlin.time.TimeSource

sealed interface ConnectionStatus {
    data object Disconnected : ConnectionStatus
    data object Scanning : ConnectionStatus
    data object Connecting : ConnectionStatus

    /** Connected; device info and catalogues are being fetched. The jacket can already be controlled. */
    data object Syncing : ConnectionStatus
    data object Ready : ConnectionStatus
    data class Failed(val reason: ConnectionFailure) : ConnectionStatus
}

enum class ConnectionFailure { BluetoothOff, PermissionMissing, MtuTooLow, UnsupportedProtocol, NoReply }

/** What the jacket can play, as it described itself. Filled page by page; [complete] once everything arrived. */
data class Catalog(
    val effects: List<EffectInfo> = emptyList(),
    val palettes: List<PaletteInfo> = emptyList(),
    val complete: Boolean = false,
)

/**
 * The app's single view of the jacket. Owns the link (real or demo), keeps the latest state, device info and
 * catalogue as [StateFlow]s, and turns user [Change]s into writes.
 *
 * Writes come in two kinds:
 * - [preview], for a control being dragged: conflated per control (the latest value wins), written without
 *   response and no more often than [PREVIEW_INTERVAL];
 * - [commit], for the end of a gesture or a plain tap: written with response, in order, never dropped.
 *
 * Both show at once in [state]. While a control is dragged, the jacket's own reports for that field are ignored;
 * after the commit they stay ignored until the jacket reports the committed value (or [CONFIRM_TIMEOUT] passes),
 * so a STATE that was already on its way cannot make the control jump back.
 *
 * Presets are not optimistic: [savePreset], [loadPreset] and [deletePreset] just send, and [presets] and
 * [state] change when the jacket answers. A command the jacket rejects surfaces in [error].
 *
 * Not thread-safe by design: construct it and call it on [scope]'s single-threaded dispatcher (the main thread).
 */
class JacketRepository(
    private val scope: CoroutineScope,
    private val linkFactory: (demo: Boolean) -> JacketLink,
    demoMode: Boolean = false,
    private val timeSource: TimeSource = TimeSource.Monotonic,
) {
    private val _connection = MutableStateFlow<ConnectionStatus>(ConnectionStatus.Disconnected)
    val connection: StateFlow<ConnectionStatus> = _connection.asStateFlow()

    private val _state = MutableStateFlow<JacketState?>(null)

    /** The jacket's state with the user's unconfirmed changes on top. Null until the first STATE arrives. */
    val state: StateFlow<JacketState?> = _state.asStateFlow()

    private val _info = MutableStateFlow<DeviceInfo?>(null)
    val info: StateFlow<DeviceInfo?> = _info.asStateFlow()

    private val _catalog = MutableStateFlow(Catalog())
    val catalog: StateFlow<Catalog> = _catalog.asStateFlow()

    private val _presets = MutableStateFlow<List<PresetInfo>?>(null)

    /** The used preset slots, as last listed by the jacket. Null until it has listed them once. */
    val presets: StateFlow<List<PresetInfo>?> = _presets.asStateFlow()

    private val _error = MutableStateFlow<ErrorReply?>(null)

    /** The last command the jacket rejected, until [dismissError]. Failed queries of the app itself are not here. */
    val error: StateFlow<ErrorReply?> = _error.asStateFlow()

    private val _demoMode = MutableStateFlow(demoMode)
    val demoMode: StateFlow<Boolean> = _demoMode.asStateFlow()

    private var link: JacketLink = linkFactory(demoMode)
    private var session: Job

    /** Set when this class, not the link, gave up on the jacket; shown instead of the link's plain "disconnected". */
    private var failure: ConnectionFailure? = null

    private var remote: JacketState? = null
    private val overrides = LinkedHashMap<Int, Override>()
    private val previews = LinkedHashMap<Int, List<Command>>()
    private val commits = ArrayDeque<Command>()
    private var identifyDragging = false
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private val replies = MutableSharedFlow<Notification>(extraBufferCapacity = 16, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    private var highPriority = false
    private var priorityHeld = false
    private var priorityRelease: Job? = null

    /** [opcode] is the command that carries the change, so an ERROR for it can undo the pretence at once. */
    private class Override(val change: Change, val dragging: Boolean, val opcode: Int)

    init {
        // Last on purpose: attaching runs coroutines right away, and they touch the fields above.
        session = attach(link)
    }

    // ---- connection -------------------------------------------------------------------------------------------

    /** Starts connecting, or does nothing when already connected or on the way. The link retries by itself. */
    fun connect() {
        failure = null
        link.connect()
    }

    fun disconnect() = link.disconnect()

    /** Swaps the Bluetooth link for the in-app fake jacket, or back. Everything known about the old one is dropped. */
    fun setDemoMode(enabled: Boolean) {
        if (enabled == _demoMode.value) return
        session.cancel()
        link.close()
        clearPendingWork()
        failure = null
        remote = null
        _state.value = null
        _info.value = null
        _catalog.value = Catalog()
        _presets.value = null
        _error.value = null
        _connection.value = ConnectionStatus.Disconnected
        _demoMode.value = enabled
        link = linkFactory(enabled)
        session = attach(link)
    }

    private fun attach(link: JacketLink): Job = scope.launch(start = CoroutineStart.UNDISPATCHED) {
        // Undispatched: the frame collector must be subscribed before anyone can call connect().
        launch(start = CoroutineStart.UNDISPATCHED) { link.frames.collect { onFrame(link, it) } }
        launch { writeLoop(link) }
        link.status.collectLatest { onLinkStatus(link, it) }
    }

    private suspend fun onLinkStatus(link: JacketLink, status: LinkStatus) {
        if (status != LinkStatus.Connected) clearPendingWork()
        _connection.value = when (status) {
            LinkStatus.Disconnected -> failure?.let(ConnectionStatus::Failed) ?: ConnectionStatus.Disconnected
            LinkStatus.Scanning -> ConnectionStatus.Scanning
            LinkStatus.Connecting -> ConnectionStatus.Connecting
            LinkStatus.Connected -> ConnectionStatus.Syncing
            is LinkStatus.Failed -> ConnectionStatus.Failed(
                when (status.reason) {
                    LinkFailure.BluetoothOff -> ConnectionFailure.BluetoothOff
                    LinkFailure.PermissionMissing -> ConnectionFailure.PermissionMissing
                    LinkFailure.MtuTooLow -> ConnectionFailure.MtuTooLow
                },
            )
        }
        if (status != LinkStatus.Connected) return
        // A fresh connection starts balanced: ask again if live mode still holds the short interval.
        if (highPriority) link.setHighPriority(true) else updatePriority()
        try {
            sync(link)
            _connection.value = ConnectionStatus.Ready
        } catch (e: CancellationException) {
            throw e // The link dropped mid-sync; collectLatest is already handling the new status.
        } catch (e: Exception) {
            giveUp(link, ConnectionFailure.NoReply)
            return
        }
        try {
            // Not part of the document's connect sequence, and not worth a connection: if the list does not
            // come, the presets screen says so and offers to ask again.
            request(link, Command.GetPresets) { it as? PresetList }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            _presets.value = null
        }
    }

    private fun giveUp(link: JacketLink, reason: ConnectionFailure) {
        failure = reason
        link.disconnect()
    }

    // ---- reading ----------------------------------------------------------------------------------------------

    private fun onFrame(link: JacketLink, frame: ByteArray) {
        val notification = try {
            Codec.decodeNotification(frame)
        } catch (e: UnsupportedProtocolException) {
            giveUp(link, ConnectionFailure.UnsupportedProtocol)
            return
        } catch (e: FrameException) {
            return // Garbage or a notification type from a newer firmware: not ours to understand.
        }
        when (notification) {
            is JacketState -> {
                remote = notification
                // A committed change is settled once the jacket reports it. Dragged ones stay until released.
                overrides.values.removeAll { !it.dragging && it.change.isReflectedIn(notification) }
                publish()
                return
            }
            // Sent after every save and delete as well as on request: the list is never polled.
            is PresetList -> _presets.value = notification.presets
            is ErrorReply -> if (!isQuery(notification.opcode)) {
                _error.value = notification
                // The change it refused will never be confirmed: show the jacket's truth again now.
                if (overrides.values.removeAll { !it.dragging && it.opcode == notification.opcode }) publish()
            }
            else -> Unit
        }
        replies.tryEmit(notification)
    }

    private fun isQuery(opcode: Int) = opcode == Opcode.GET_INFO || opcode == Opcode.GET_CATALOG || opcode == Opcode.GET_PRESETS

    private fun publish() {
        _state.value = remote?.let { overrides.values.fold(it) { state, override -> override.change.applyTo(state) } }
    }

    /** The connect sequence of the document, after MTU, subscribe and the first read: info, then the catalogues. */
    private suspend fun sync(link: JacketLink) {
        _info.value = request(link, Command.GetInfo) { it as? DeviceInfo }

        // First time: show each page as it lands. On a reconnect keep the old catalogue up until the new one is whole.
        val progressive = !_catalog.value.complete
        val effects = fetchCatalog(link, CatalogKind.EFFECTS) {
            if (progressive) _catalog.value = Catalog(effects = it.filterIsInstance<EffectInfo>())
        }.filterIsInstance<EffectInfo>()
        val palettes = fetchCatalog(link, CatalogKind.PALETTES) {
            if (progressive) _catalog.value = Catalog(effects, it.filterIsInstance<PaletteInfo>())
        }.filterIsInstance<PaletteInfo>()
        _catalog.value = Catalog(effects, palettes, complete = true)
    }

    private suspend fun fetchCatalog(link: JacketLink, kind: Int, onPage: (List<CatalogEntry>) -> Unit): List<CatalogEntry> {
        val entries = ArrayList<CatalogEntry>()
        while (true) {
            val start = entries.size
            val page = request(link, Command.GetCatalog(kind, start)) { reply ->
                (reply as? CatalogPage)?.takeIf { it.kind == kind && it.start == start }
            }
            entries += page.entries
            onPage(entries)
            // An empty page before `total` would loop forever; treat it as the end.
            if (entries.size >= page.total || page.entries.isEmpty()) return entries
        }
    }

    /** Sends a `GET_*` command and waits for the notification [pick] accepts, asking again if nothing comes. */
    private suspend fun <T : Any> request(link: JacketLink, command: Command, pick: (Notification) -> T?): T {
        val frame = Codec.encode(command)
        repeat(REQUEST_ATTEMPTS) {
            val reply = withTimeoutOrNull(REQUEST_TIMEOUT) {
                replies
                    .onSubscription {
                        try {
                            link.send(frame, withResponse = true)
                        } catch (e: IOException) {
                            // Most likely the link is dropping, and its status will cancel us. If it is not,
                            // no reply comes and the timeout below takes care of it.
                        }
                    }
                    .mapNotNull { reply ->
                        if (reply is ErrorReply && reply.opcode == command.opcode) {
                            throw IllegalStateException("jacket answered error ${reply.code} to $command")
                        }
                        pick(reply)
                    }
                    .first()
            }
            if (reply != null) return reply
        }
        throw IllegalStateException("no reply to $command")
    }

    // ---- writing ----------------------------------------------------------------------------------------------

    /** A control is being dragged to [change]. Cheap: call it for every value the gesture produces. */
    fun preview(change: Change) {
        val shown = _state.value
        if (shown == null || !canWrite()) return
        val key = change.key
        val commands = change.toCommands(shown)
        overrides[key] = Override(change, dragging = true, commands[0].opcode)
        previews[key] = commands
        wake.trySend(Unit)
        publish()
        updatePriority()
    }

    /** The gesture ended on [change], or the user tapped something. Delivered reliably, after any earlier commit. */
    fun commit(change: Change) {
        val shown = _state.value
        if (shown == null || !canWrite()) return
        val key = change.key
        // Against what is shown, not what was last reported: earlier commits will have landed by then.
        val commands = change.toCommands(shown)
        val override = Override(change, dragging = false, commands[0].opcode)
        overrides[key] = override
        previews.remove(key)
        commits.addAll(commands)
        wake.trySend(Unit)
        publish()
        updatePriority()
        scope.launch {
            delay(CONFIRM_TIMEOUT)
            // Never confirmed (clamped, rejected or lost): stop pretending and show what the jacket says.
            if (overrides[key] === override) {
                overrides.remove(key)
                publish()
            }
        }
    }

    /**
     * Replaces the jacket's segments by [segments], each playing the effect it carries. Shown at once, like any
     * commit. The caller validates first (see [Zones.problems]); what the jacket still refuses comes back in [error].
     */
    fun applyLayout(segments: List<Segment>) = commit(Change.Layout(segments))

    /**
     * Lights [length] pixels from [start] on the strips of [stripMask] for [seconds], over whatever is playing;
     * zero [length] or [seconds] cancels. With [dragging] the range is still being moved: such calls are
     * conflated and unacknowledged like any preview, and the call that ends the drag goes out reliably.
     */
    fun identify(stripMask: Int, start: Int, length: Int, seconds: Int, dragging: Boolean = false) {
        if (!canWrite()) return
        val command = Command.Identify(stripMask, start, length, seconds)
        previews.remove(KEY_IDENTIFY)
        if (dragging) previews[KEY_IDENTIFY] = listOf(command) else commits.addLast(command)
        identifyDragging = dragging
        wake.trySend(Unit)
        updatePriority()
    }

    /** Stores the current scene in [slot] under [name], at most 16 bytes of UTF-8. The jacket answers with the new list. */
    fun savePreset(slot: Int, name: String) {
        require(name.utf8Size <= Protocol.MAX_PRESET_NAME_BYTES) { "preset name longer than ${Protocol.MAX_PRESET_NAME_BYTES} bytes" }
        send(Command.PresetSave(slot, name))
    }

    fun loadPreset(slot: Int) = send(Command.PresetLoad(slot))

    fun deletePreset(slot: Int) = send(Command.PresetDelete(slot))

    /** Asks for the preset list again, for when the one asked for at connect never came. */
    fun refreshPresets() = send(Command.GetPresets)

    fun dismissError() {
        _error.value = null
    }

    /** A command with nothing to show optimistically: queued behind the commits, delivered reliably. */
    private fun send(command: Command) {
        if (!canWrite()) return
        commits.addLast(command)
        wake.trySend(Unit)
    }

    /**
     * One frame to the `live` characteristic, unacknowledged. It does not queue behind the commands: live input
     * is only worth sending while it is fresh. Throws [IOException] when the jacket is not connected.
     */
    suspend fun sendLive(frame: LiveFrame) = link.sendLive(Codec.encode(frame))

    /**
     * Keeps the short connection interval for as long as [hold] stays true, across reconnects: live mode sends
     * thirty frames a second and each should wait as little as possible for its turn. Dragging a control asks
     * for the same thing by itself; the interval is released when neither wants it any more.
     */
    fun holdHighPriority(hold: Boolean) {
        if (priorityHeld == hold) return
        priorityHeld = hold
        updatePriority()
    }

    private fun canWrite() = remote != null && link.status.value == LinkStatus.Connected

    /** The only writer of user changes, so a commit can never overtake the previews that came before it. */
    private suspend fun writeLoop(link: JacketLink) {
        var nextPreview = timeSource.markNow()
        while (true) {
            wake.receive()
            while (true) {
                val commit = commits.removeFirstOrNull()
                if (commit != null) {
                    write(link, commit, withResponse = true)
                    continue
                }
                if (previews.isEmpty()) break
                val wait = -nextPreview.elapsedNow()
                if (wait > Duration.ZERO) {
                    delay(wait)
                    continue // A commit may have arrived meanwhile; it goes first.
                }
                val batch = previews.values.flatten()
                previews.clear()
                nextPreview = timeSource.markNow() + PREVIEW_INTERVAL
                for (command in batch) write(link, command, withResponse = false)
            }
        }
    }

    private suspend fun write(link: JacketLink, command: Command, withResponse: Boolean) {
        try {
            link.send(Codec.encode(command), withResponse)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // The link is going down; its status says so and the pending work is cleared there.
        }
    }

    private fun clearPendingWork() {
        previews.clear()
        commits.clear()
        overrides.clear()
        identifyDragging = false
        // No drag survives this, so only live mode can still want the short interval. The link is told either
        // way: it would otherwise ask for it again on the next connection, with nobody left to release it.
        priorityRelease?.cancel()
        priorityRelease = null
        if (highPriority && !priorityHeld) {
            highPriority = false
            link.setHighPriority(false)
        }
        publish()
    }

    /**
     * High connection priority while something is dragged or live mode holds it, released a little later so two
     * drags share one request.
     */
    private fun updatePriority() {
        val wanted = priorityHeld || identifyDragging || overrides.values.any { it.dragging }
        if (wanted) {
            priorityRelease?.cancel()
            priorityRelease = null
            if (!highPriority) {
                highPriority = true
                link.setHighPriority(true)
            }
        } else if (highPriority && priorityRelease == null) {
            priorityRelease = scope.launch {
                delay(PRIORITY_LINGER)
                highPriority = false
                priorityRelease = null
                link.setHighPriority(false)
            }
        }
    }

    companion object {
        /** About 30 writes per second while dragging. */
        val PREVIEW_INTERVAL = 33.milliseconds
        val CONFIRM_TIMEOUT = 1500.milliseconds
        val PRIORITY_LINGER = 3.seconds
        val REQUEST_TIMEOUT = 2.seconds
        const val REQUEST_ATTEMPTS = 3
        private const val KEY_IDENTIFY = 6
    }
}
