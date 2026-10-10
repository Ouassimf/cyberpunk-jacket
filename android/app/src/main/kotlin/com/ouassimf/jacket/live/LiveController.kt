package com.ouassimf.jacket.live

import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.yield
import kotlin.time.Duration.Companion.minutes
import kotlin.time.Duration.Companion.nanoseconds

enum class LiveStatus {
    Off,

    /** Running, and frames are going to the jacket. */
    Streaming,

    /** Running, but the jacket is not connected: still sensing, nothing is sent until it is back. */
    Waiting,
}

/** Something live mode did by itself, or could not do, that the user should be told. */
enum class LiveNotice {
    /** The jacket stayed away for [LiveController.IDLE_TIMEOUT], so live mode stopped to free the microphone. */
    IdleTimeout,

    /** The microphone could not be opened; live mode goes on without sound. */
    MicrophoneUnavailable,

    /** Android did not let live mode start. */
    StartRefused,
}

/**
 * Live mode itself: while running it samples the analysers' results thirty times a second, turns them into a
 * frame and hands it to the repository, which writes it to the jacket without waiting for an answer.
 *
 * - It only sends while the jacket is connected and synced. When the jacket drops out it keeps sampling (the
 *   meters stay alive) and resumes by itself when the jacket is back; if that takes [IDLE_TIMEOUT] it stops, so
 *   a forgotten session does not hold the microphone for ever.
 * - While it runs it holds the short Bluetooth connection interval, so a frame waits milliseconds, not tens of
 *   them, for its turn on the air.
 * - [stop] is immediate: no frame is sent after it returns, and the jacket clears its live flag 500 ms later.
 * - A frame that takes longer to write than a frame lasts is not made up for: late frames are skipped, never
 *   queued, so what the jacket gets is always fresh.
 *
 * It knows nothing about microphones or sensors: whoever captures (the foreground service) writes to [features]
 * from its own threads. Not thread-safe otherwise: construct it and call it on [scope]'s single-threaded
 * dispatcher (the main thread), like the repository.
 *
 * @param nanoTime the monotonic clock the results in [features] are stamped with.
 */
class LiveController(
    private val scope: CoroutineScope,
    private val repository: JacketRepository,
    val features: LiveFeatures,
    initial: LiveSettings = LiveSettings(),
    private val nanoTime: () -> Long = System::nanoTime,
) {
    private val _settings = MutableStateFlow(initial.sanitized())
    val settings: StateFlow<LiveSettings> = _settings.asStateFlow()

    private val _status = MutableStateFlow(LiveStatus.Off)
    val status: StateFlow<LiveStatus> = _status.asStateFlow()

    private val _notice = MutableStateFlow<LiveNotice?>(null)

    /** The last thing worth telling the user, until the next [start] or [dismissNotice]. */
    val notice: StateFlow<LiveNotice?> = _notice.asStateFlow()

    /** What the last tick sampled. Plain fields: read them when [onMeters] says they changed. */
    val meters = LiveMeters()

    /** Called on the main thread after every tick, and once when live mode stops. For the screen's meters. */
    var onMeters: (() -> Unit)? = null

    /** Frames handed to the link since construction. */
    var framesSent = 0L
        private set

    private val builder = LiveFrameBuilder()
    private var job: Job? = null

    val running: Boolean get() = _status.value != LiveStatus.Off

    fun update(transform: (LiveSettings) -> LiveSettings) {
        val next = transform(_settings.value).sanitized()
        _settings.value = next
        if (running && !next.anySource) stop()
    }

    /** Starts streaming, or does nothing when already running or when both sound and motion are switched off. */
    fun start() {
        if (running || !_settings.value.anySource) return
        _notice.value = null
        _status.value = if (online()) LiveStatus.Streaming else LiveStatus.Waiting
        repository.holdHighPriority(true)
        job = scope.launch { run() }
    }

    /** Stops streaming at once. [because] is shown to the user when live mode did not stop at their request. */
    fun stop(because: LiveNotice? = null) {
        if (because != null) _notice.value = because
        if (!running) return
        job?.cancel()
        job = null
        _status.value = LiveStatus.Off
        repository.holdHighPriority(false)
        meters.clear()
        onMeters?.invoke()
    }

    /** Something to tell the user while live mode goes on. */
    fun report(notice: LiveNotice) {
        _notice.value = notice
    }

    fun dismissNotice() {
        _notice.value = null
    }

    private fun online() = repository.connection.value == ConnectionStatus.Ready

    private suspend fun run() {
        var due = nanoTime()
        var offline = false
        var offlineSince = 0L
        while (true) {
            val now = nanoTime()
            val online = online()
            if (online) {
                offline = false
            } else {
                if (!offline) {
                    offline = true
                    offlineSince = now
                }
                if (now - offlineSince >= IDLE_TIMEOUT.inWholeNanoseconds) {
                    stop(LiveNotice.IdleTimeout)
                    return
                }
            }
            _status.value = if (online) LiveStatus.Streaming else LiveStatus.Waiting

            builder.sample(features, _settings.value, now, meters)
            onMeters?.invoke()
            if (online) {
                try {
                    repository.sendLive(builder.frame(meters, now))
                    framesSent++
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    // The link is going down; the connection status says so in a moment and the frame is stale anyway.
                }
            }

            due += FRAME_INTERVAL_NANOS
            val wait = due - nanoTime()
            if (wait > 0) {
                delay(wait.nanoseconds)
            } else {
                due = nanoTime() // fell behind: skip, never catch up with a burst
                yield()
            }
        }
    }

    companion object {
        const val FRAMES_PER_SECOND = 30
        const val FRAME_INTERVAL_NANOS = 1_000_000_000L / FRAMES_PER_SECOND

        /** How long live mode waits for a jacket that went away before it gives up and frees the microphone. */
        val IDLE_TIMEOUT = 5.minutes
    }
}
