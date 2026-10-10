package com.ouassimf.jacket.live

import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.link.FakeJacket
import com.ouassimf.jacket.link.LinkStatus
import com.ouassimf.jacket.link.RecordingLink
import com.ouassimf.jacket.link.ScriptedLink
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.LiveFrame
import com.ouassimf.jacket.protocol.Protocol
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import kotlin.time.Duration.Companion.minutes
import kotlin.time.Duration.Companion.seconds

@OptIn(ExperimentalCoroutinesApi::class)
class LiveControllerTest {

    /** Live mode on a fake jacket, with a clock that is the test's own and analysers that always have news. */
    private class Bench(private val scope: TestScope, settings: LiveSettings = LiveSettings()) {
        val jacket = FakeJacket(scope.backgroundScope)
        val link = RecordingLink(jacket)
        val repository = JacketRepository(scope.backgroundScope, { link }, timeSource = scope.testScheduler.timeSource)
        val features = LiveFeatures()
        val now: Long get() = scope.testScheduler.currentTime * BeatClock.NANOS_PER_MILLI
        val live = LiveController(scope.backgroundScope, repository, features, settings, nanoTime = { now })
        val frames: List<LiveFrame> get() = link.live
        val jacketSeesLive: Boolean get() = repository.state.value!!.liveInput
        var ticks = 0

        init {
            live.onMeters = { ticks++ }
            // A microphone and an accelerometer that deliver every 20 ms.
            scope.backgroundScope.launch {
                while (true) {
                    features.audio(now, 200, 150, 100, 50, beatValid = true, bpm = 120f, beatPhase = (now / 1e6 % 500 / 500).toFloat(), beatPeriodSeconds = 0.5f)
                    features.motion(now, 77, -10, 20)
                    delay(20)
                }
            }
        }
    }

    private fun connected(settings: LiveSettings = LiveSettings(latencyOffsetMs = 0), block: suspend Bench.() -> Unit) = runTest {
        val bench = Bench(this, settings)
        bench.repository.connect()
        delay(1_000)
        assertEquals(ConnectionStatus.Ready, bench.repository.connection.value)
        bench.block()
    }

    @Test
    fun `it streams thirty frames a second, numbered and timestamped`() = connected {
        assertEquals(LiveStatus.Off, live.status.value)
        assertFalse(jacketSeesLive)
        val startedAt = now / BeatClock.NANOS_PER_MILLI
        live.start()
        assertEquals(LiveStatus.Streaming, live.status.value)
        delay(10_010)

        assertEquals("frames in ten seconds, counting both ends", 301, frames.size)
        assertEquals(frames.indices.map { it and 0xFF }, frames.map { it.seq })
        assertEquals(frames.size.toLong(), live.framesSent)
        val times = frames.map { it.tMs }
        assertEquals("the first frame leaves at once", (startedAt and 0xFFFF).toInt(), times[0])
        assertTrue("33 or 34 ms apart: ${times.zipWithNext { a, b -> b - a }.distinct()}", times.zipWithNext { a, b -> b - a }.all { it in 33..34 })
        assertTrue("no drift: frame 300 is ten seconds after frame 0", times[300] - times[0] == 10_000)
        assertTrue(frames.all { it.flags == 7 && it.level == 200 && it.motion == 77 && it.tiltX == -10 && it.bpmX10 == 1200 })
        assertTrue("the jacket's live flag is up", jacketSeesLive)
        assertEquals(frames.size, ticks)
    }

    @Test
    fun `the phase in each frame is the beat at the moment it leaves`() = connected {
        live.start()
        delay(2_000)
        // The fake microphone says: phase = (time mod 500 ms) / 500 ms. A frame must say the same of its own t_ms.
        fun assertPhase(expected: Int, frame: LiveFrame) {
            val off = (frame.beatPhase - expected + 256) % 256
            assertTrue("frame at ${frame.tMs}: phase ${frame.beatPhase}, expected $expected", off <= 1 || off == 255)
        }
        for (frame in frames) assertPhase(frame.tMs % 500 * 256 / 500, frame)

        link.live.clear()
        live.update { it.copy(latencyOffsetMs = 125) } // a quarter of this beat
        delay(2_000)
        for (frame in frames) assertPhase((frame.tMs + 125) % 500 * 256 / 500, frame)
        assertTrue(frames.size >= 59)
    }

    @Test
    fun `after stop nothing more is sent and the jacket's live flag clears`() = connected {
        live.start()
        delay(1_000)
        live.stop()
        assertEquals(LiveStatus.Off, live.status.value)
        val sent = frames.size
        delay(Protocol.LIVE_TIMEOUT_MS - 50)
        assertTrue("still up 450 ms after the last frame", jacketSeesLive)
        delay(300)
        assertFalse("cleared by the jacket's own timeout", jacketSeesLive)
        delay(60_000)
        assertEquals("not one frame after stop", sent, frames.size)
        assertEquals("the meters went dark", 0, live.meters.level)
        assertFalse(live.meters.audioValid)
        assertNull(live.notice.value)

        live.start() // and it can start again, counting on
        delay(1_000)
        assertEquals(sent and 0xFF, frames[sent].seq)
        assertTrue(jacketSeesLive)
    }

    @Test
    fun `high connection priority is held while streaming and released after`() = connected {
        live.start()
        assertEquals(listOf(true), link.priorities)
        delay(60_000)
        assertEquals("for as long as it runs", listOf(true), link.priorities)
        live.stop()
        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds + 100)
        assertEquals(listOf(true, false), link.priorities)
    }

    @Test
    fun `when the jacket drops out it waits without sending, and resumes when it is back`() = connected {
        live.start()
        delay(1_000)
        jacket.simulateDrop()
        delay(100)
        assertEquals(LiveStatus.Waiting, live.status.value)
        val sent = frames.size
        val ticksBefore = ticks
        delay(200)
        assertEquals("nothing is sent into the void", sent, frames.size)
        assertTrue("but it keeps sampling, the meters stay alive", ticks - ticksBefore in 5..7)
        assertTrue(live.meters.audioValid && live.meters.level == 200)

        delay(1_000) // the fake jacket is back after 350 ms, and synced a little later
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(LiveStatus.Streaming, live.status.value)
        assertTrue("streaming again: ${frames.size - sent} frames", frames.size - sent >= 15)
        assertEquals("numbering goes on where it stopped", (0 until frames.size).map { it and 0xFF }, frames.map { it.seq })
        assertTrue(jacketSeesLive)
        assertEquals("the new connection was asked for high priority again", listOf(true, true), link.priorities)
    }

    @Test
    fun `it does not send while the jacket is still being read after a connect`() = runTest {
        val bench = Bench(this)
        bench.live.start() // before anything is connected
        assertEquals(LiveStatus.Waiting, bench.live.status.value)
        bench.repository.connect()
        var sentWhileNotReady = 0
        repeat(100) {
            delay(10)
            if (bench.repository.connection.value != ConnectionStatus.Ready) sentWhileNotReady = bench.frames.size
        }
        assertEquals(0, sentWhileNotReady)
        assertEquals(LiveStatus.Streaming, bench.live.status.value)
        assertTrue(bench.frames.isNotEmpty())
    }

    @Test
    fun `it gives up after five minutes without the jacket`() = connected {
        live.start()
        delay(1_000)
        repository.disconnect()
        delay(100)
        val sent = frames.size
        delay(LiveController.IDLE_TIMEOUT - 1.seconds)
        assertEquals(LiveStatus.Waiting, live.status.value)
        delay(2.seconds)
        assertEquals(LiveStatus.Off, live.status.value)
        assertEquals(LiveNotice.IdleTimeout, live.notice.value)
        assertEquals(sent, frames.size)

        // Even if the jacket comes back now: off is off.
        val ticksAtStop = ticks
        repository.connect()
        delay(10_000)
        assertEquals(sent, frames.size)
        assertEquals(ticksAtStop, ticks)
        delay(JacketRepository.PRIORITY_LINGER)
        assertEquals("priority released", false, link.priorities.last())

        live.start()
        assertNull("a new start clears the notice", live.notice.value)
    }

    @Test
    fun `a jacket that comes back in time restarts the five minutes`() = connected {
        live.start()
        repeat(3) {
            repository.disconnect()
            delay(4.minutes)
            assertEquals(LiveStatus.Waiting, live.status.value)
            repository.connect()
            delay(2.seconds)
            assertEquals(LiveStatus.Streaming, live.status.value)
        }
        repository.disconnect()
        delay(4.minutes + 59.seconds)
        assertEquals(LiveStatus.Waiting, live.status.value)
        delay(2.seconds)
        assertEquals(LiveStatus.Off, live.status.value)
    }

    @Test
    fun `settings take effect in the next frame`() = connected {
        live.start()
        delay(500)
        assertEquals(7, frames.last().flags)
        live.update { it.copy(sound = false) }
        delay(100)
        assertEquals(LiveFrame.FLAG_MOTION, frames.last().flags)
        assertEquals(0, frames.last().level)
        live.update { it.copy(sound = true, motion = false) }
        delay(100)
        assertEquals(LiveFrame.FLAG_AUDIO or LiveFrame.FLAG_BEAT, frames.last().flags)
        assertEquals(0, frames.last().motion)
        live.update { it.copy(sensitivity = 500, latencyOffsetMs = -5000) }
        assertEquals(LiveSettings(sound = true, motion = false, sensitivity = 100, latencyOffsetMs = -200), live.settings.value)
    }

    @Test
    fun `with sound and motion both off there is nothing to stream`() = connected(LiveSettings(sound = false, motion = false)) {
        live.start()
        assertEquals(LiveStatus.Off, live.status.value)
        delay(1_000)
        assertTrue(frames.isEmpty())
        assertTrue(link.priorities.isEmpty())

        live.update { it.copy(motion = true) }
        live.start()
        delay(1_000)
        assertEquals(LiveStatus.Streaming, live.status.value)
        live.update { it.copy(motion = false) } // the last source goes: so does live mode
        assertEquals(LiveStatus.Off, live.status.value)
        val sent = frames.size
        delay(1_000)
        assertEquals(sent, frames.size)
    }

    @Test
    fun `starting twice starts once`() = connected {
        live.start()
        live.start()
        delay(1_000)
        assertTrue("${frames.size} frames in a second", frames.size in 30..31)
        assertEquals(listOf(true), link.priorities)
        live.stop()
        live.stop()
        assertEquals(LiveStatus.Off, live.status.value)
    }

    @Test
    fun `a notice can be shown while it runs and stays until dismissed or the next start`() = connected {
        live.start()
        live.report(LiveNotice.MicrophoneUnavailable)
        delay(500)
        assertEquals(LiveStatus.Streaming, live.status.value)
        assertEquals(LiveNotice.MicrophoneUnavailable, live.notice.value)
        live.dismissNotice()
        assertNull(live.notice.value)
        live.stop(LiveNotice.StartRefused)
        assertEquals(LiveNotice.StartRefused, live.notice.value)
        live.stop(LiveNotice.IdleTimeout) // already off: the reason is still worth showing
        assertEquals(LiveNotice.IdleTimeout, live.notice.value)
    }

    @Test
    fun `switching to another jacket mid-stream carries on with the new one`() = runTest {
        val jackets = ArrayList<RecordingLink>()
        val repository = JacketRepository(backgroundScope, { RecordingLink(FakeJacket(backgroundScope)).also { jackets += it } }, timeSource = testScheduler.timeSource)
        val live = LiveController(backgroundScope, repository, LiveFeatures(), nanoTime = { testScheduler.currentTime * BeatClock.NANOS_PER_MILLI })
        repository.connect()
        delay(1_000)
        live.start()
        delay(1_000)
        repository.setDemoMode(true)
        assertEquals(LiveStatus.Streaming, live.status.value) // until the next tick notices
        delay(100)
        assertEquals(LiveStatus.Waiting, live.status.value)
        repository.connect()
        delay(1_000)
        assertEquals(LiveStatus.Streaming, live.status.value)
        assertEquals(2, jackets.size)
        assertTrue(jackets[1].live.isNotEmpty())
        assertEquals("seq goes on across the switch", (jackets[0].live.last().seq + 1) and 0xFF, jackets[1].live.first().seq)
        assertEquals(listOf(true), jackets[1].priorities)
        assertTrue(repository.state.value!!.liveInput)
    }

    // ---- against a link that misbehaves -------------------------------------------------------------------------

    private class Scripted(scope: TestScope) {
        val link = ScriptedLink()
        val repository = JacketRepository(scope.backgroundScope, { link }, timeSource = scope.testScheduler.timeSource)
        val live = LiveController(scope.backgroundScope, repository, LiveFeatures(), nanoTime = { scope.testScheduler.currentTime * BeatClock.NANOS_PER_MILLI })

        suspend fun connect() {
            repository.connect()
            link.status.value = LinkStatus.Connected
            link.notify(JacketState(Protocol.VERSION, 1, true, 128, 900, 0, Protocol.NO_PRESET, FakeJacketSegments))
            delay(100)
            assertEquals(ConnectionStatus.Ready, repository.connection.value)
        }
    }

    @Test
    fun `nothing is sent while the app is still reading the jacket`() = runTest {
        // A link that is up but a jacket that has not answered the first questions yet: six seconds of that.
        val bench = Scripted(this)
        bench.link.onCommand = {}
        bench.live.start()
        bench.repository.connect()
        bench.link.status.value = LinkStatus.Connected
        bench.link.notify(JacketState(Protocol.VERSION, 1, true, 128, 900, 0, Protocol.NO_PRESET, FakeJacketSegments))
        delay(5_000)
        assertEquals(ConnectionStatus.Syncing, bench.repository.connection.value)
        assertEquals(LiveStatus.Waiting, bench.live.status.value)
        assertTrue("${bench.link.live.size} frames sent into a jacket that is not ready", bench.link.live.isEmpty())
    }

    @Test
    fun `a slow link gets fewer frames, never a queue of old ones`() = runTest {
        val bench = Scripted(this)
        bench.connect()
        bench.link.onLive = { delay(100) } // each write takes three frame times
        bench.live.start()
        delay(3_000)
        val slow = bench.link.live.toList()
        assertTrue("${slow.size} frames in three seconds", slow.size in 28..31)
        assertEquals("every frame that was built was sent, in order", slow.indices.toList(), slow.map { it.seq })
        assertTrue("each was fresh when it was built: ${slow.zipWithNext { a, b -> b.tMs - a.tMs }.distinct()}", slow.zipWithNext { a, b -> b.tMs - a.tMs }.all { it == 100 })

        bench.link.onLive = {}
        bench.link.live.clear()
        delay(1_000)
        assertTrue("back to thirty a second without a burst: ${bench.link.live.size}", bench.link.live.size in 29..31)
        assertTrue(bench.link.live.drop(2).zipWithNext { a, b -> b.tMs - a.tMs }.all { it in 33..34 })
    }

    @Test
    fun `a write that fails costs that frame and nothing else`() = runTest {
        val bench = Scripted(this)
        bench.connect()
        var failing = true
        bench.link.onLive = { if (failing) throw IOException("radio hiccup") }
        bench.live.start()
        delay(500)
        assertEquals(LiveStatus.Streaming, bench.live.status.value)
        assertTrue(bench.link.live.isEmpty())
        assertEquals(0L, bench.live.framesSent)
        failing = false
        delay(500)
        assertTrue("${bench.link.live.size} frames in half a second", bench.link.live.size in 14..16)
        assertTrue("the failed frames used up their numbers, so the jacket can count the loss", bench.link.live.first().seq in 14..16)
    }

    @Test
    fun `stop during a write that hangs sends nothing afterwards`() = runTest {
        val bench = Scripted(this)
        bench.connect()
        bench.link.onLive = { delay(10_000) }
        bench.live.start()
        delay(100)
        bench.live.stop()
        delay(60_000)
        assertTrue("the write in flight was abandoned", bench.link.live.isEmpty())
        assertEquals(listOf(true, false), bench.link.priorities)
    }

    private companion object {
        val FakeJacketSegments = listOf(
            com.ouassimf.jacket.protocol.Segment(
                com.ouassimf.jacket.protocol.SegmentGeometry(3, 0, 144, 0),
                com.ouassimf.jacket.protocol.EffectConfig(0, 0, 128, 128, com.ouassimf.jacket.protocol.Rgb(1), com.ouassimf.jacket.protocol.Rgb(2)),
            ),
        )
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class ConnectionLifetimeTest {

    private class Bench(scope: TestScope) {
        var disconnects = 0
        val lifetime = ConnectionLifetime(scope.backgroundScope, 20.seconds) { disconnects++ }
    }

    @Test
    fun `with the screens gone the jacket is let go after the grace period, once`() = runTest {
        val bench = Bench(this)
        bench.lifetime.onUiStarted()
        delay(1.minutes)
        assertEquals("never while the app is on screen", 0, bench.disconnects)
        bench.lifetime.onUiStopped()
        delay(19.seconds)
        assertEquals(0, bench.disconnects)
        delay(2.seconds)
        assertEquals(1, bench.disconnects)
        delay(10.minutes)
        assertEquals(1, bench.disconnects)
    }

    @Test
    fun `a quick trip to another app costs nothing`() = runTest {
        val bench = Bench(this)
        bench.lifetime.onUiStarted()
        bench.lifetime.onUiStopped()
        delay(19.seconds)
        bench.lifetime.onUiStarted()
        delay(1.minutes)
        assertEquals(0, bench.disconnects)
        bench.lifetime.onUiStopped() // and the next trip gets its own full grace period
        delay(19.seconds)
        assertEquals(0, bench.disconnects)
        delay(2.seconds)
        assertEquals(1, bench.disconnects)
    }

    @Test
    fun `live mode keeps the jacket for as long as it runs, and the grace period starts when it ends`() = runTest {
        val bench = Bench(this)
        bench.lifetime.onUiStarted()
        bench.lifetime.onLiveChanged(true)
        bench.lifetime.onUiStopped()
        delay(3.minutes * 60)
        assertEquals("three hours with the screen off", 0, bench.disconnects)
        bench.lifetime.onLiveChanged(false)
        delay(19.seconds)
        assertEquals(0, bench.disconnects)
        delay(2.seconds)
        assertEquals(1, bench.disconnects)
    }

    @Test
    fun `live mode started during the grace period cancels it, and ending it on screen does nothing`() = runTest {
        val bench = Bench(this)
        bench.lifetime.onUiStarted()
        bench.lifetime.onUiStopped()
        delay(10.seconds)
        bench.lifetime.onLiveChanged(true)
        delay(1.minutes)
        assertEquals(0, bench.disconnects)
        bench.lifetime.onUiStarted()
        bench.lifetime.onLiveChanged(false)
        bench.lifetime.onLiveChanged(false)
        delay(10.minutes)
        assertEquals(0, bench.disconnects)
    }

    @Test
    fun `every time the app is left for long the jacket is let go again`() = runTest {
        val bench = Bench(this)
        repeat(3) { round ->
            bench.lifetime.onUiStarted()
            delay(5.seconds)
            bench.lifetime.onUiStopped()
            delay(21.seconds)
            assertEquals(round + 1, bench.disconnects)
        }
        bench.lifetime.onLiveChanged(true)
        bench.lifetime.onLiveChanged(false)
        delay(21.seconds)
        assertEquals("and after a live session in the background too", 4, bench.disconnects)
    }

    @Test
    fun `leaving twice does not shorten or double the wait`() = runTest {
        val bench = Bench(this)
        bench.lifetime.onUiStarted()
        bench.lifetime.onUiStopped()
        delay(15.seconds)
        bench.lifetime.onUiStopped() // e.g. a second activity stop
        bench.lifetime.onLiveChanged(false)
        delay(6.seconds)
        assertEquals(1, bench.disconnects)
        delay(1.minutes)
        assertEquals(1, bench.disconnects)
    }
}
