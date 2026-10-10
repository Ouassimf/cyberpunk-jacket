package com.ouassimf.jacket.analysis

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.floor
import kotlin.random.Random

/** The tracker alone, on onset trains written by hand: one value per frame, 100 frames a second. */
class BeatTrackerTest {

    /** [seconds] of onsets; pulses are placed at fractional frame positions by splitting them over two frames. */
    private class Train(seconds: Int) {
        val frames = FloatArray(seconds * BeatTracker.FRAME_RATE)

        fun pulse(at: Double, strength: Float = 1f) = apply {
            val i = floor(at).toInt()
            val part = (at - i).toFloat()
            if (i in frames.indices) frames[i] += strength * (1 - part)
            if (i + 1 in frames.indices) frames[i + 1] += strength * part
        }

        /** Pulses every beat of [bpm] from frame [first]; [strengths] are used in turn. */
        fun beat(bpm: Double, first: Double = 12.3, from: Int = 0, to: Int = frames.size, vararg strengths: Float = floatArrayOf(1f)) = apply {
            val period = 6000 / bpm
            var at = first
            var n = 0
            while (at < to) {
                if (at >= from) pulse(at, strengths[n % strengths.size])
                at += period
                n++
            }
        }

        fun noise(amount: Float, random: Random) = apply {
            for (i in frames.indices) frames[i] += amount * random.nextFloat()
        }
    }

    private fun BeatTracker.feed(train: Train, from: Int = 0, to: Int = train.frames.size, each: (frame: Int) -> Unit = {}) {
        for (i in from until to) {
            add(train.frames[i])
            each(i)
        }
    }

    /** Phase error at the latest frame [frame], in frames, against a beat of [bpm] that has a pulse at [first]. */
    private fun phaseError(tracker: BeatTracker, frame: Int, bpm: Double, first: Double): Double {
        val period = 6000 / bpm
        val truth = (frame - first) / period
        var error = tracker.phase - (truth - floor(truth))
        error -= floor(error + 0.5)
        return error * period
    }

    @Test
    fun `a metronome is followed in tempo and phase at every tempo in range`() {
        for (bpm in listOf(70.0, 77.7, 90.0, 100.0, 113.0, 120.0, 128.0, 140.0, 150.0, 163.5, 174.0, 180.0)) {
            val tracker = BeatTracker()
            val train = Train(20).beat(bpm)
            var lockedAt = -1
            tracker.feed(train) { frame ->
                if (lockedAt < 0 && tracker.valid) {
                    lockedAt = frame
                    assertTrue("$bpm BPM: phase off by ${phaseError(tracker, frame, bpm, 12.3)} frames when the beat is first announced", abs(phaseError(tracker, frame, bpm, 12.3)) < 1.0)
                }
                if (frame >= 600) {
                    assertTrue("$bpm BPM lost at frame $frame", tracker.valid)
                    assertEquals("$bpm BPM at frame $frame", bpm, tracker.bpm.toDouble(), 0.5)
                    assertTrue("$bpm BPM: phase off by ${phaseError(tracker, frame, bpm, 12.3)} frames at $frame", abs(phaseError(tracker, frame, bpm, 12.3)) < 0.5)
                }
            }
            assertTrue("$bpm BPM locked at frame $lockedAt", lockedAt in 349..550)
            assertTrue("$bpm BPM: confidence ${tracker.confidence}", tracker.confidence > 0.7f)
        }
    }

    @Test
    fun `phase zero is the beat itself`() {
        val tracker = BeatTracker()
        // Pulses on whole frames, every 50: at a pulse frame the phase must be 0 (or just under 1), half-way 0.5.
        val train = Train(12).beat(120.0, first = 20.0)
        val phases = FloatArray(train.frames.size)
        tracker.feed(train) { phases[it] = tracker.phase }
        fun distance(a: Float, b: Float) = abs(a - b).let { minOf(it, 1 - it) }
        for (pulse in listOf(820, 870, 1120)) {
            assertTrue("phase ${phases[pulse]} on the beat", distance(phases[pulse], 0f) < 0.01f)
            assertTrue("phase ${phases[pulse + 25]} half-way", distance(phases[pulse + 25], 0.5f) < 0.01f)
            assertTrue("phase ${phases[pulse + 49]} just before the next", distance(phases[pulse + 49], 0.98f) < 0.01f)
        }
    }

    @Test
    fun `nothing is claimed before there is enough to go on`() {
        val tracker = BeatTracker()
        val train = Train(10).beat(120.0)
        tracker.feed(train, to = 340) {
            assertFalse(tracker.valid)
            assertEquals(0f, tracker.bpm)
        }
    }

    @Test
    fun `equal pulses are the beat, weaker ones in between are not`() {
        // Every 34.5 frames, all the same: 174 BPM.
        val equal = BeatTracker().apply { feed(Train(15).beat(174.0)) }
        assertEquals(174.0, equal.bpm.toDouble(), 0.5)

        // The same pulses, every second one at 40 %: the strong ones are the beat, 87 BPM, and phase 0 is on them.
        val tracker = BeatTracker()
        val train = Train(15).beat(174.0, strengths = floatArrayOf(1f, 0.4f))
        tracker.feed(train)
        assertTrue(tracker.valid)
        assertEquals(87.0, tracker.bpm.toDouble(), 0.5)
        assertTrue("phase off by ${phaseError(tracker, 1499, 87.0, 12.3)} frames", abs(phaseError(tracker, 1499, 87.0, 12.3)) < 0.5)

        // Nearly as strong (90 %) counts as equal.
        val nearly = BeatTracker().apply { feed(Train(15).beat(174.0, strengths = floatArrayOf(1f, 0.9f))) }
        assertEquals(174.0, nearly.bpm.toDouble(), 0.5)
    }

    @Test
    fun `a slow beat with weak off-beats is not doubled, at either end of the range`() {
        for (bpm in listOf(72.0, 90.0)) {
            val tracker = BeatTracker()
            val train = Train(15).beat(bpm).beat(bpm, first = 12.3 + 3000 / bpm, strengths = floatArrayOf(0.35f))
            tracker.feed(train)
            assertEquals(bpm, tracker.bpm.toDouble(), 0.5)
        }
    }

    @Test
    fun `tempos outside the range come out as their half or double`() {
        val fast = BeatTracker().apply { feed(Train(15).beat(220.0)) }
        assertTrue(fast.valid)
        assertEquals(110.0, fast.bpm.toDouble(), 0.5)
        val slow = BeatTracker().apply { feed(Train(15).beat(55.0)) }
        assertTrue(slow.valid)
        assertEquals(110.0, slow.bpm.toDouble(), 0.5)
    }

    @Test
    fun `a three-against-two pattern keeps the beat`() {
        // Beats at 120 with a pulse on every third one twice as strong: still 120, not 80 or 40.
        val tracker = BeatTracker().apply { feed(Train(15).beat(120.0, strengths = floatArrayOf(2f, 1f, 1f))) }
        assertEquals(120.0, tracker.bpm.toDouble(), 0.5)
    }

    @Test
    fun `a lock holds through small noise and loose timing`() {
        val random = Random(1)
        val train = Train(30).noise(0.15f, random)
        var at = 9.0
        while (at < 3000) {
            train.pulse(at + random.nextDouble() * 1.6 - 0.8, 0.7f + 0.6f * random.nextFloat())
            at += 6000 / 126.0
        }
        val tracker = BeatTracker()
        tracker.feed(train) { frame ->
            if (frame >= 600) {
                assertTrue("lost at frame $frame", tracker.valid)
                assertEquals(126.0, tracker.bpm.toDouble(), 1.0)
                assertTrue(abs(phaseError(tracker, frame, 126.0, 9.0)) < 1.5)
            }
        }
    }

    @Test
    fun `a tempo that drifts is followed all the way`() {
        // A band that speeds up from 120 to 127 BPM over twenty seconds, never by enough at once to be a new tempo.
        val beats = ArrayList<Double>()
        var at = 10.0
        while (at < 4000) {
            beats += at
            val bpm = 120 + 7 * ((at - 1000) / 2000).coerceIn(0.0, 1.0)
            at += 6000 / bpm
        }
        val train = Train(40)
        for (beat in beats) train.pulse(beat)
        val tracker = BeatTracker()
        tracker.feed(train) { frame ->
            if (frame < 600) return@feed
            assertTrue("lost at frame $frame", tracker.valid)
            val next = beats.indexOfFirst { it > frame }
            if (next <= 0) return@feed
            val period = beats[next] - beats[next - 1]
            // The tempo heard over the last six seconds is that of about three seconds ago: at most 1.1 BPM behind.
            assertEquals("tempo at frame $frame", 6000 / period, tracker.bpm.toDouble(), 1.5)
            var error = tracker.phase - (frame - beats[next - 1]) / period
            error -= floor(error + 0.5)
            // While the tempo keeps rising the oscillator trails the beat a little: under 20 ms.
            assertTrue("phase off by ${error * period} frames at $frame", abs(error * period) < 2.0)
        }
        assertEquals(127.0, tracker.bpm.toDouble(), 0.5)
    }

    @Test
    fun `a beat is found on top of a level that swells and sinks`() {
        // Onsets riding on a busy, slowly changing background five times their size: only what stands out of
        // its surroundings may count, or the background's own shape drowns the beat.
        val random = Random(6)
        val train = Train(30).beat(120.0).noise(0.2f, random)
        for (i in train.frames.indices) train.frames[i] += 2.5f + 2.5f * kotlin.math.sin(i / 100f)
        val tracker = BeatTracker()
        tracker.feed(train) { frame ->
            if (frame >= 600) {
                assertTrue("lost at frame $frame", tracker.valid)
                assertEquals(120.0, tracker.bpm.toDouble(), 0.5)
                assertTrue(abs(phaseError(tracker, frame, 120.0, 12.3)) < 1.0)
            }
        }
    }

    @Test
    fun `when noise takes over from the beat, the beat is gone within three seconds`() {
        // The music stops and something loud but shapeless goes on: the six seconds of history still hold more
        // beat than noise for a while. The last seconds on their own do not.
        val random = Random(7)
        val train = Train(30).beat(120.0, to = 1500)
        for (i in 1500 until train.frames.size) train.frames[i] = random.nextFloat()
        val tracker = BeatTracker()
        var lastValid = 0
        tracker.feed(train) { if (tracker.valid) lastValid = it }
        assertTrue("valid until frame $lastValid", lastValid in 1500..1800)
    }

    @Test
    fun `a new tempo replaces the old one, and no third tempo shows on the way`() {
        val train = Train(40).beat(100.0, to = 2000).beat(150.0, first = 2004.0, from = 2000)
        val tracker = BeatTracker()
        tracker.feed(train, to = 2000)
        assertEquals(100.0, tracker.bpm.toDouble(), 0.5)
        var switchedAt = -1
        tracker.feed(train, from = 2000) { frame ->
            if (tracker.valid) assertTrue("${tracker.bpm} BPM at frame $frame", abs(tracker.bpm - 100) < 2 || abs(tracker.bpm - 150) < 2)
            if (switchedAt < 0 && tracker.valid && abs(tracker.bpm - 150) < 2) switchedAt = frame
            if (frame >= 2600) {
                assertTrue(tracker.valid)
                assertEquals(150.0, tracker.bpm.toDouble(), 0.5)
                assertTrue(abs(phaseError(tracker, frame, 150.0, 2004.0)) < 0.5)
            }
        }
        assertTrue("switched at frame $switchedAt", switchedAt in 2200..2600)
    }

    @Test
    fun `a jump in phase at the same tempo is followed after a moment, not at once`() {
        // The same tempo, but from frame 1500 on the beats come 20 frames later (40 % of a beat).
        val train = Train(30).beat(120.0, first = 10.0, to = 1500).beat(120.0, first = 1530.0, from = 1500)
        val tracker = BeatTracker()
        tracker.feed(train, to = 1500)
        assertTrue(abs(phaseError(tracker, 1499, 120.0, 10.0)) < 0.5)
        var followedAt = -1
        tracker.feed(train, from = 1500) { frame ->
            if (followedAt < 0 && tracker.valid && abs(phaseError(tracker, frame, 120.0, 1530.0)) < 1.0) followedAt = frame
            if (frame >= 2100) assertTrue("phase at frame $frame", tracker.valid && abs(phaseError(tracker, frame, 120.0, 1530.0)) < 0.5)
        }
        assertTrue("followed at frame $followedAt", followedAt in 1600..2100)
    }

    @Test
    fun `when the pulses stop the beat is gone within three seconds, and back quickly when they return`() {
        val train = Train(30).beat(120.0, to = 1000).beat(120.0, first = 2012.3, from = 2000)
        val tracker = BeatTracker()
        var lastValid = 0
        tracker.feed(train, to = 2000) { if (tracker.valid) lastValid = it }
        assertTrue("valid until frame $lastValid", lastValid in 1000..1300)
        assertFalse(tracker.valid)
        assertEquals("the tempo is remembered for a while, for a quicker lock", 120.0, tracker.bpm.toDouble(), 0.5)

        var backAt = -1
        tracker.feed(train, from = 2000) { if (backAt < 0 && tracker.valid) backAt = it }
        assertTrue("back at frame $backAt", backAt in 2000..2500)
    }

    @Test
    fun `a beat that stays away is forgotten`() {
        val train = Train(30).beat(120.0, to = 1000)
        val tracker = BeatTracker().apply { feed(train) }
        assertFalse(tracker.valid)
        assertEquals(0f, tracker.bpm)
        assertEquals(0f, tracker.periodFrames)
    }

    @Test
    fun `silence, a constant, noise, a ramp and random hits have no beat`() {
        fun assertNoBeat(name: String, train: Train) {
            val tracker = BeatTracker()
            var highest = 0f
            tracker.feed(train) { frame ->
                assertFalse("$name got ${tracker.bpm} BPM at frame $frame", tracker.valid)
                highest = maxOf(highest, tracker.confidence)
            }
            assertTrue("$name: confidence $highest", highest < BeatTracker.MIN_CONFIDENCE)
        }
        assertNoBeat("silence", Train(20))
        assertNoBeat("a constant", Train(20).apply { frames.fill(3f) })
        assertNoBeat("noise", Train(40).noise(1f, Random(2)))
        // A level that only rises correlates with itself at every lag; it must not pass for a beat.
        assertNoBeat("a ramp", Train(40).noise(0.3f, Random(3)).apply { for (i in frames.indices) frames[i] += i * 0.01f })
        assertNoBeat("a ramp that restarts", Train(40).apply { for (i in frames.indices) frames[i] = (i % 700) * 0.01f })
    }

    @Test
    fun `hits at random moments are almost never taken for a beat`() {
        // Random hits do line up by accident now and then, and for a second or two that is a beat to anyone's
        // ear. What can be asked is that it is rare: here, under half a percent of the time over 100 runs of 40 s.
        var valid = 0
        var total = 0
        for (seed in 1..100) {
            val random = Random(seed)
            val train = Train(40)
            var at = 5.0
            while (at < 4000) {
                train.pulse(at)
                at += 15 + random.nextDouble() * 90 // 0.15 to 1.05 s apart
            }
            val tracker = BeatTracker()
            tracker.feed(train) {
                total++
                if (tracker.valid) valid++
            }
        }
        assertTrue("a beat was claimed in $valid of $total frames", valid < total / 300)
    }

    @Test
    fun `hits that are only roughly regular are a beat`() {
        // Every 0.4 to 0.5 s, at random: sloppy, but a pulse of about 133 BPM all the same.
        val random = Random(4)
        val train = Train(40)
        var at = 5.0
        while (at < 4000) {
            train.pulse(at)
            at += 40 + random.nextDouble() * 10
        }
        val tracker = BeatTracker()
        var valid = 0
        tracker.feed(train) { if (tracker.valid) { valid++; assertEquals(133.0, tracker.bpm.toDouble(), 12.0) } }
        assertTrue("valid in $valid of 4000 frames", valid > 1000)
    }

    @Test
    fun `reset forgets the beat`() {
        val tracker = BeatTracker().apply { feed(Train(10).beat(120.0)) }
        assertTrue(tracker.valid)
        tracker.reset()
        assertFalse(tracker.valid)
        assertEquals(0f, tracker.bpm)
        assertEquals(0f, tracker.phase)
        tracker.feed(Train(3).beat(120.0))
        assertFalse("three seconds are not enough again", tracker.valid)
    }
}
