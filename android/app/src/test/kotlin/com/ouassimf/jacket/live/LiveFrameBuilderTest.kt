package com.ouassimf.jacket.live

import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.LiveFrame
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class BeatClockTest {
    private val ms = BeatClock.NANOS_PER_MILLI
    private val beat = 500 * ms // 120 BPM

    @Test
    fun `the phase moves on with time, one turn per beat`() {
        assertEquals(0.25, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 1000 * ms), 1e-9)
        assertEquals(0.45, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 1100 * ms), 1e-6)
        assertEquals(0.95, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 1350 * ms), 1e-6)
        assertEquals("wraps into the next beat", 0.05, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 1400 * ms), 1e-6)
        assertEquals("many beats later", 0.25, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 1000 * ms + 1000 * beat), 1e-6)
        assertEquals("and before the measurement", 0.05, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 900 * ms), 1e-6)
        assertEquals("wrapping backwards", 0.85, BeatClock.phaseAt(0.25f, 1000 * ms, beat, 800 * ms), 1e-6)
        assertEquals("a slower beat moves less", 0.35, BeatClock.phaseAt(0.25f, 1000 * ms, 1000 * ms, 1100 * ms), 1e-6)
    }

    @Test
    fun `a positive offset sends the phase the beat will have, a negative one the phase it had`() {
        val now = 7_000 * ms
        fun sent(offset: Int) = BeatClock.phaseToSend(0.5f, now, beat, now, offset)
        assertEquals(0.5, sent(0), 1e-9)
        assertEquals("100 ms ahead is a fifth of this beat", 0.7, sent(100), 1e-6)
        assertEquals(0.3, sent(-100), 1e-6)
        assertEquals("ahead across the beat", 0.1, sent(300), 1e-6)
        assertEquals("back across the beat", 0.9, sent(-300), 1e-6)
        assertEquals("the whole range of the slider", 0.1, sent(-200), 1e-6)
        assertEquals("a whole beat changes nothing", 0.5, sent(500), 1e-6)
        // The offset is in time: the same 100 ms is less of a slow beat than of a fast one.
        assertEquals(0.6, BeatClock.phaseToSend(0.5f, now, 1000 * ms, now, 100), 1e-6)
        assertEquals(0.8, BeatClock.phaseToSend(0.5f, now, 333_333_333, now, 100), 1e-6)
    }

    @Test
    fun `the phase is always inside the beat`() {
        for (phase in listOf(0f, 0.001f, 0.5f, 0.999f, 0.99999994f)) {
            for (elapsed in listOf(-3_000_000_007L, -beat, -1L, 0L, 1L, beat - 1, beat, 86_400_000 * ms)) {
                for (offset in listOf(-200, -1, 0, 1, 300)) {
                    val sent = BeatClock.phaseToSend(phase, 0, beat, elapsed, offset)
                    assertTrue("$sent for phase $phase after $elapsed ns with offset $offset", sent >= 0.0 && sent < 1.0)
                    assertTrue(BeatClock.toByte(sent) in 0..255)
                }
            }
        }
        // An instant so close under a beat that the arithmetic rounds it onto the beat: that is phase 0, not 1.
        assertEquals(0.0, BeatClock.phaseAt(0f, 0, 1_000_000_000_000_000_000, -1), 0.0)
        assertEquals(0, BeatClock.toByte(BeatClock.phaseAt(0f, 0, 1_000_000_000_000_000_000, -1)))
        assertThrows(IllegalArgumentException::class.java) { BeatClock.phaseAt(0f, 0, 0, 1) }
    }

    @Test
    fun `the byte is 0 on the beat, 128 half-way and 255 just before the next`() {
        assertEquals(0, BeatClock.toByte(0.0))
        assertEquals(0, BeatClock.toByte(0.0039))
        assertEquals(1, BeatClock.toByte(0.004))
        assertEquals(64, BeatClock.toByte(0.25))
        assertEquals(128, BeatClock.toByte(0.5))
        assertEquals(255, BeatClock.toByte(0.9999))
        assertEquals("never 256", 255, BeatClock.toByte(1.0))
    }
}

class LiveFrameBuilderTest {
    private val ms = BeatClock.NANOS_PER_MILLI
    private val features = LiveFeatures()
    private val builder = LiveFrameBuilder()
    private val meters = LiveMeters()

    private fun hear(at: Long, beat: Boolean = true, phase: Float = 0.25f, bpm: Float = 120f) =
        features.audio(at, level = 200, bass = 150, mid = 100, treble = 50, beatValid = beat, bpm = bpm, beatPhase = phase, beatPeriodSeconds = 60f / bpm)

    private fun move(at: Long) = features.motion(at, motion = 77, tiltX = -100, tiltY = 127)

    /** With no latency offset unless the test asks for one: what is sent is then the beat as heard. */
    private fun frame(now: Long, settings: LiveSettings = LiveSettings(latencyOffsetMs = 0)): LiveFrame {
        builder.sample(features, settings, now, meters)
        return builder.frame(meters, now)
    }

    @Test
    fun `with nothing sensed yet every flag is clear and every field zero`() {
        assertEquals(LiveFrame(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, tMs = 5000), frame(5000 * ms))
    }

    @Test
    fun `a frame carries what was sensed, with the phase carried to the moment it is built`() {
        hear(at = 10_000 * ms)
        move(at = 10_040 * ms)
        val frame = frame(10_100 * ms)
        assertEquals(LiveFrame.FLAG_AUDIO or LiveFrame.FLAG_MOTION or LiveFrame.FLAG_BEAT, frame.flags)
        assertEquals(listOf(200, 150, 100, 50), listOf(frame.level, frame.bass, frame.mid, frame.treble))
        assertEquals(1200, frame.bpmX10)
        assertEquals("0.25 of a beat when heard, 100 ms of a 500 ms beat later: 0.45", 115, frame.beatPhase)
        assertEquals(listOf(77, -100, 127), listOf(frame.motion, frame.tiltX, frame.tiltY))
        assertEquals(10_100, frame.tMs)
        assertEquals(0.45f, meters.heardPhase, 1e-4f)
        assertEquals(14, Codec.encode(frame).size)
    }

    @Test
    fun `the latency offset shifts the phase that is sent and nothing else`() {
        hear(at = 10_000 * ms)
        move(at = 10_000 * ms)
        val plain = frame(10_100 * ms)
        val usual = frame(10_100 * ms, LiveSettings())
        assertEquals("the default of 30 ms: 0.51", 130, usual.beatPhase)
        val ahead = frame(10_100 * ms, LiveSettings(latencyOffsetMs = 50))
        val behind = frame(10_100 * ms, LiveSettings(latencyOffsetMs = -200))
        assertEquals(115, plain.beatPhase)
        assertEquals("0.55", 140, ahead.beatPhase)
        assertEquals("0.05", 12, behind.beatPhase)
        assertEquals(plain.copy(seq = 0, beatPhase = 0), ahead.copy(seq = 0, beatPhase = 0))
        assertEquals(plain.copy(seq = 0, beatPhase = 0), behind.copy(seq = 0, beatPhase = 0))
        assertEquals("what the screen's beat light follows is the beat as heard", 0.45f, meters.heardPhase, 1e-4f)
    }

    @Test
    fun `without a beat the beat fields are zero and the levels still go out`() {
        hear(at = 1000 * ms, beat = false)
        val frame = frame(1010 * ms)
        assertEquals(LiveFrame.FLAG_AUDIO, frame.flags)
        assertEquals(200, frame.level)
        assertEquals(0, frame.bpmX10)
        assertEquals(0, frame.beatPhase)
    }

    @Test
    fun `a source that is switched off is not sent, however fresh`() {
        hear(at = 1000 * ms)
        move(at = 1000 * ms)
        val silent = frame(1010 * ms, LiveSettings(sound = false, latencyOffsetMs = 0))
        assertEquals(LiveFrame.FLAG_MOTION, silent.flags)
        assertEquals(listOf(0, 0, 0, 0, 0, 0), listOf(silent.level, silent.bass, silent.mid, silent.treble, silent.bpmX10, silent.beatPhase))
        assertEquals(77, silent.motion)

        val still = frame(1010 * ms, LiveSettings(motion = false))
        assertEquals(LiveFrame.FLAG_AUDIO or LiveFrame.FLAG_BEAT, still.flags)
        assertEquals(listOf(0, 0, 0), listOf(still.motion, still.tiltX, still.tiltY))
        assertEquals(200, still.level)
    }

    @Test
    fun `results that stopped coming are dropped after 400 ms`() {
        hear(at = 1000 * ms)
        move(at = 1200 * ms)
        assertEquals(7, frame(1400 * ms).flags)
        assertEquals("the microphone stalled", LiveFrame.FLAG_MOTION, frame(1401 * ms).flags)
        assertEquals(LiveFrame.FLAG_MOTION, frame(1600 * ms).flags)
        assertEquals(0, frame(1601 * ms).flags)
        hear(at = 1700 * ms)
        assertEquals("and are back when they are", LiveFrame.FLAG_AUDIO or LiveFrame.FLAG_BEAT, frame(1710 * ms).flags)
    }

    @Test
    fun `a capture that ended is cleared at once`() {
        hear(at = 1000 * ms)
        move(at = 1000 * ms)
        features.clearAudio()
        assertEquals(LiveFrame.FLAG_MOTION, frame(1001 * ms).flags)
        features.clearMotion()
        assertEquals(0, frame(1002 * ms).flags)
    }

    @Test
    fun `seq counts frames and wraps at 256, t_ms wraps at 65536`() {
        val frames = (0 until 600).map { frame((65_000L + it * 33) * ms) }
        assertEquals((0 until 600).map { it and 0xFF }, frames.map { it.seq })
        assertEquals((0 until 600).map { ((65_000 + it * 33) and 0xFFFF) }, frames.map { it.tMs })
        assertTrue("t_ms did wrap", frames.zipWithNext().count { (a, b) -> b.tMs < a.tMs } == 1)
        // Sampling without building a frame uses no number: nothing was sent.
        builder.sample(features, LiveSettings(), 0, meters)
        assertEquals(600 and 0xFF, builder.frame(meters, 0).seq)
    }

    @Test
    fun `a frame carries the peak since the frame before, so a hit between two frames is not lost`() {
        // The analyser reports every 10 or 20 ms, a frame leaves every 33: three reports, the middle one a hit.
        features.audio(1000 * ms, level = 40, bass = 30, mid = 20, treble = 10, beatValid = false, bpm = 0f, beatPhase = 0f, beatPeriodSeconds = 0f)
        features.audio(1010 * ms, level = 250, bass = 240, mid = 90, treble = 80, beatValid = false, bpm = 0f, beatPhase = 0f, beatPeriodSeconds = 0f)
        features.audio(1020 * ms, level = 120, bass = 110, mid = 100, treble = 20, beatValid = false, bpm = 0f, beatPhase = 0f, beatPeriodSeconds = 0f)
        features.motion(1000 * ms, motion = 10, tiltX = 5, tiltY = 6)
        features.motion(1010 * ms, motion = 200, tiltX = 50, tiltY = 60)
        features.motion(1020 * ms, motion = 90, tiltX = 7, tiltY = 8)
        val first = frame(1030 * ms)
        assertEquals("each at its own peak", listOf(250, 240, 100, 80), listOf(first.level, first.bass, first.mid, first.treble))
        assertEquals(200, first.motion)
        assertEquals("tilt is a position, not a strength: the latest", listOf(7, 8), listOf(first.tiltX, first.tiltY))

        // Nothing new by the next frame: it says where things stand, not the old peak again.
        val second = frame(1063 * ms)
        assertEquals(listOf(120, 110, 100, 20), listOf(second.level, second.bass, second.mid, second.treble))
        assertEquals(90, second.motion)

        features.audio(1070 * ms, level = 60, bass = 0, mid = 0, treble = 0, beatValid = false, bpm = 0f, beatPhase = 0f, beatPeriodSeconds = 0f)
        assertEquals("a fall is sent as soon as it is all there was", 60, frame(1096 * ms).level)
    }

    @Test
    fun `tempo is tenths of a BPM, rounded`() {
        hear(at = 0, bpm = 127.96f)
        assertEquals(1280, frame(1).bpmX10)
        hear(at = 0, bpm = 174.04f)
        assertEquals(1740, frame(1).bpmX10)
        hear(at = 0, bpm = 70.25f)
        assertEquals(703, frame(1).bpmX10) // 702.5 rounds up
    }

    @Test
    fun `every frame it can build fits the wire format`() {
        // Values an analyser should never produce still must not make the codec throw at 30 Hz.
        features.audio(0, level = 999, bass = -5, mid = 256, treble = 255, beatValid = true, bpm = 9999f, beatPhase = 0.99999f, beatPeriodSeconds = 0.006f)
        features.motion(0, motion = 300, tiltX = -128, tiltY = 500)
        val frame = frame(1)
        assertEquals(listOf(255, 0, 255, 255), listOf(frame.level, frame.bass, frame.mid, frame.treble))
        assertEquals(listOf(255, -127, 127), listOf(frame.motion, frame.tiltX, frame.tiltY))
        assertEquals(0xFFFF, frame.bpmX10)
        assertEquals(frame, Codec.decodeLive(Codec.encode(frame)))

        features.audio(0, 1, 1, 1, 1, beatValid = true, bpm = 120f, beatPhase = 0f, beatPeriodSeconds = 0f)
        assertEquals("a beat without a period is no beat", LiveFrame.FLAG_AUDIO or LiveFrame.FLAG_MOTION, frame(1).flags)
    }

    @Test
    fun `settings read from storage are brought into range`() {
        assertEquals(LiveSettings(sensitivity = 100, latencyOffsetMs = 300), LiveSettings(sensitivity = 5000, latencyOffsetMs = 9999).sanitized())
        assertEquals(LiveSettings(sensitivity = 0, latencyOffsetMs = -200), LiveSettings(sensitivity = -1, latencyOffsetMs = -201).sanitized())
        assertEquals(LiveSettings(), LiveSettings().sanitized())
        assertEquals("out of the box the beat is sent 30 ms ahead, for the way to the LEDs", 30, LiveSettings().latencyOffsetMs)
        assertTrue(LiveSettings.DEFAULT_OFFSET_MS in LiveSettings.OFFSET_RANGE_MS)
    }
}
