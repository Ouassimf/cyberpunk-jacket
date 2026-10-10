package com.ouassimf.jacket.analysis

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.lang.management.ManagementFactory
import kotlin.math.abs
import kotlin.math.floor
import kotlin.random.Random

/**
 * The analyser against sound made for the purpose, where the right answer is known exactly.
 *
 * What these tests state about the beat: within [CONVERGENCE] seconds of a steady beat starting, the tempo is
 * within [TEMPO_TOLERANCE] BPM and the phase within [PHASE_TOLERANCE_MS] of the truth, and stays there.
 * They say nothing about real music through a real microphone; that takes ears and a jacket.
 */
class AudioAnalyzerTest {

    // ---- levels ------------------------------------------------------------------------------------------------

    @Test
    fun `silence is level zero in every band, with no onset and no beat`() {
        val analyzer = AudioAnalyzer()
        analyzer.feed(Mix(10.0).pcm()) {
            assertEquals(0, analyzer.level)
            assertEquals(0, analyzer.bass)
            assertEquals(0, analyzer.mid)
            assertEquals(0, analyzer.treble)
            assertFalse(analyzer.beatValid)
        }
        assertEquals(0f, analyzer.bpm)
        assertEquals(0L, analyzer.onsets)
        assertEquals(1000L, analyzer.frames)
    }

    @Test
    fun `the hiss of a quiet room stays dark`() {
        // About -75 dB below full scale: what a phone microphone gives in a silent room.
        val analyzer = AudioAnalyzer()
        var highest = 0
        analyzer.feed(Mix(20.0).noise(0.0003, Random(1)).pcm()) {
            highest = maxOf(highest, analyzer.level, analyzer.bass, analyzer.mid, analyzer.treble)
            assertFalse(analyzer.beatValid)
        }
        assertTrue("hiss read as $highest", highest <= 8)
    }

    @Test
    fun `a steady tone lights its own band and no other`() {
        fun bands(hz: Double): List<Int> {
            val analyzer = AudioAnalyzer()
            analyzer.feed(Mix(3.0).tone(hz, 0.1).pcm())
            assertTrue("level ${analyzer.level} for $hz Hz", analyzer.level >= 240)
            return listOf(analyzer.bass, analyzer.mid, analyzer.treble)
        }
        for ((hz, band) in listOf(60.0 to 0, 100.0 to 0, 440.0 to 1, 1000.0 to 1, 1700.0 to 1, 3000.0 to 2, 6000.0 to 2)) {
            val values = bands(hz)
            assertTrue("$hz Hz gave $values", values[band] >= 240)
            for (other in values.indices) if (other != band) assertTrue("$hz Hz gave $values", values[other] <= 5)
        }
    }

    @Test
    fun `a band follows its own sound when two play at once`() {
        // Bass steady, treble switched on half-way and 20 dB weaker: bass must not move, treble must appear.
        val analyzer = AudioAnalyzer()
        val pcm = Mix(8.0).tone(90.0, 0.2).tone(5000.0, 0.02, from = 4.0).pcm()
        var trebleBefore = 0
        analyzer.feed(pcm) { t -> if (t in 3.0..4.0) trebleBefore = maxOf(trebleBefore, analyzer.treble) }
        assertEquals(0, trebleBefore)
        assertTrue("bass ${analyzer.bass}", analyzer.bass >= 240)
        assertTrue("treble ${analyzer.treble} gets its own range although it is 20 dB weaker", analyzer.treble >= 200)
        assertTrue("mid ${analyzer.mid}", analyzer.mid <= 5)
    }

    @Test
    fun `a hit shows at once and is over when the sound is, for the jacket to smooth as it likes`() {
        val analyzer = AudioAnalyzer()
        val random = Random(2)
        // A click of a few milliseconds, then a kick (which itself rings for a good 100 ms).
        val mix = Mix(6.0).kick(4.0, 0.5, random)
        for (i in 0 until 48) mix.data[2 * mix.rate + i] += 0.5f * (random.nextFloat() * 2 - 1)
        val levels = ArrayList<Int>()
        analyzer.feed(mix.pcm(), block = AudioAnalyzer.HOP) { levels += analyzer.level }
        fun at(seconds: Double) = levels[(seconds * 100).toInt()]
        assertEquals(0, at(1.99))
        assertTrue("20 ms after the click: ${at(2.02)}", at(2.02) >= 200)
        // The click is in the 32 ms analysis window for three frames; after that only 30 ms of release are left.
        assertTrue("100 ms after the click: ${at(2.10)}", at(2.10) <= 60)
        assertTrue("200 ms after the click: ${at(2.20)}", at(2.20) <= 3)

        assertEquals(0, at(3.95))
        assertTrue("30 ms after the kick: ${at(4.03)}", at(4.03) >= 200)
        assertTrue("a kick is still sounding 60 ms on: ${at(4.06)}", at(4.06) >= 60)
        assertTrue("and has rung out 400 ms on: ${at(4.4)}", at(4.4) <= 3)
    }

    @Test
    fun `a quiet room and a loud club both use the whole range`() {
        for (amplitude in listOf(0.012, 0.9)) {
            val random = Random(3)
            val analyzer = AudioAnalyzer()
            var highest = 0
            var lowest = 255
            var bassHighest = 0
            analyzer.feed(Mix(15.0).beat(100.0, random, amplitude = amplitude).noise(amplitude / 200, random).pcm()) { t ->
                if (t >= 8.0) {
                    highest = maxOf(highest, analyzer.level)
                    lowest = minOf(lowest, analyzer.level)
                    bassHighest = maxOf(bassHighest, analyzer.bass)
                }
            }
            assertTrue("peaks reach $highest at amplitude $amplitude", highest >= 220)
            assertTrue("and fall to $lowest between hits", lowest <= 60)
            assertTrue("bass peaks reach $bassHighest", bassHighest >= 220)
        }
    }

    @Test
    fun `a hit half as loud reads half as high`() {
        // The jacket uses the values linearly, so they must be linear in what is heard: kicks at full and at
        // half amplitude in turn, once the gain has settled on the loud ones.
        val random = Random(18)
        val mix = Mix(16.0).noise(0.0005, random)
        var at = 0.5
        var loud = true
        while (at < 15.9) {
            mix.kick(at, if (loud) 0.5 else 0.25, random)
            loud = !loud
            at += 0.5
        }
        val levels = ArrayList<Int>()
        val analyzer = AudioAnalyzer()
        analyzer.feed(mix.pcm(), block = AudioAnalyzer.HOP) { levels += analyzer.level }
        fun peak(kickAt: Double) = levels.subList((kickAt * 100).toInt(), (kickAt * 100).toInt() + 15).max()
        val pairs = (6..14).map { second -> peak(second + 0.5) to peak(second + 1.0) }
        for ((full, half) in pairs) {
            assertTrue("a loud kick reads $full: $pairs", full >= 235)
            assertTrue("one half as loud reads $half next to $full: $pairs", half in 105..150)
        }
    }

    @Test
    fun `the gain follows a 20 dB change within a few seconds`() {
        // Steady noise, ten times louder from 15 s to 25 s.
        val pcm = Mix(40.0).noise(0.01, Random(4)).scale(10.0, from = 15.0, to = 25.0).pcm()
        val analyzer = AudioAnalyzer()
        val levels = ArrayList<Int>()
        analyzer.feed(pcm, block = AudioAnalyzer.HOP) { levels += analyzer.level }
        fun lowest(from: Double, to: Double) = levels.subList((from * 100).toInt(), (to * 100).toInt()).min()
        fun highest(from: Double, to: Double) = levels.subList((from * 100).toInt(), (to * 100).toInt()).max()

        assertTrue("settled before the step: ${lowest(5.0, 15.0)}", lowest(5.0, 15.0) >= 225)
        assertTrue("louder is still full, never more: ${lowest(15.0, 25.0)}", lowest(15.0, 25.0) >= 225)
        assertTrue("20 dB down is nearly dark at first: ${lowest(25.0, 26.5)}", lowest(25.0, 26.5) <= 40)
        assertTrue("and is not back within a second: ${highest(25.3, 26.0)}", highest(25.3, 26.0) <= 120)
        assertTrue("but is within five: ${lowest(30.0, 40.0)}", lowest(30.0, 40.0) >= 225)
    }

    @Test
    fun `sensitivity decides how quiet a sound may be`() {
        // A soft tone at -58 dB: below the floor at neutral sensitivity, above it at full.
        val pcm = Mix(5.0).tone(440.0, 0.0018).pcm()
        fun level(sensitivity: Int): Int {
            val analyzer = AudioAnalyzer()
            analyzer.sensitivity = sensitivity
            analyzer.feed(pcm)
            return analyzer.level
        }
        val low = level(0)
        val neutral = level(50)
        val high = level(100)
        assertTrue("$low < $neutral < $high", low < neutral && neutral < high)
        assertTrue("least sensitive: $low", low <= 8)
        assertTrue("neutral: $neutral", neutral in 40..200)
        assertEquals(255, high)
    }

    // ---- onsets --------------------------------------------------------------------------------------------------

    @Test
    fun `every kick is an onset and nothing else is`() {
        val random = Random(5)
        val analyzer = AudioAnalyzer()
        val times = ArrayList<Double>()
        analyzer.feed(Mix(12.0).beat(120.0, random, first = 1.0).noise(0.003, random).pcm(), block = AudioAnalyzer.HOP) { t ->
            if (analyzer.onset) times += t
        }
        assertEquals("one per kick: $times", 22, times.size)
        for ((i, t) in times.withIndex()) {
            val kick = 1.0 + i * 0.5
            assertTrue("onset $i at $t for the kick at $kick", t - kick in 0.0..0.04)
        }
        assertEquals(22L, analyzer.onsets)
    }

    // ---- tempo and phase ------------------------------------------------------------------------------------------

    /** The analyser's beat phase at [seconds] against the truth, in milliseconds; positive means it runs ahead. */
    private fun phaseError(analyzer: AudioAnalyzer, seconds: Double, bpm: Double, first: Double): Double {
        val period = 60 / bpm
        val truth = (seconds - first) / period
        var error = analyzer.beatPhase - (truth - floor(truth))
        error -= floor(error + 0.5)
        return error * period * 1000
    }

    private class Tracking(var lockedAt: Double = -1.0, var worstTempo: Double = 0.0, var worstPhase: Double = 0.0)

    /** Feeds [pcm] and checks tempo and phase against [bpm] in every block from [from] seconds on. */
    private fun track(pcm: ShortArray, bpm: Double, first: Double, from: Double = CONVERGENCE, to: Double = Double.MAX_VALUE, analyzer: AudioAnalyzer = AudioAnalyzer()): Tracking {
        val result = Tracking()
        analyzer.feed(pcm) { t ->
            if (result.lockedAt < 0 && analyzer.beatValid) result.lockedAt = t
            if (t in from..to) {
                assertTrue("no beat at $t s for $bpm BPM", analyzer.beatValid)
                result.worstTempo = maxOf(result.worstTempo, abs(analyzer.bpm - bpm))
                result.worstPhase = maxOf(result.worstPhase, abs(phaseError(analyzer, t, bpm, first)))
                assertEquals(60.0 / analyzer.bpm, analyzer.beatPeriod.toDouble(), 1e-4)
            }
        }
        return result
    }

    @Test
    fun `kick tracks are followed in tempo and phase across the whole range`() {
        for (bpm in listOf(70.0, 90.0, 100.0, 120.0, 128.0, 140.0, 174.0, 180.0)) {
            for (seed in 1..2) {
                val random = Random(seed)
                val first = 0.1 + random.nextDouble() * 0.5
                // Noise 30 dB under the kicks.
                val pcm = Mix(20.0).beat(bpm, random, first = first).noise(0.016, random).pcm()
                val result = track(pcm, bpm, first)
                assertTrue("$bpm BPM locked at ${result.lockedAt} s", result.lockedAt in 3.0..CONVERGENCE)
                assertTrue("$bpm BPM: tempo off by ${result.worstTempo}", result.worstTempo <= TEMPO_TOLERANCE)
                assertTrue("$bpm BPM: phase off by ${result.worstPhase} ms", result.worstPhase <= PHASE_TOLERANCE_MS)
            }
        }
    }

    @Test
    fun `hats between the kicks do not double the tempo`() {
        for (bpm in listOf(90.0, 128.0)) {
            val random = Random(6)
            val pcm = Mix(20.0).beat(bpm, random, first = 0.3, hats = 0.12).noise(0.01, random).pcm()
            val result = track(pcm, bpm, 0.3)
            assertTrue("$bpm BPM with hats: tempo off by ${result.worstTempo}", result.worstTempo <= TEMPO_TOLERANCE)
            assertTrue("$bpm BPM with hats: phase off by ${result.worstPhase} ms", result.worstPhase <= PHASE_TOLERANCE_MS)
        }
    }

    @Test
    fun `a pulse as strong between the beats as on them is the faster tempo`() {
        // Kicks every 345 ms are 174 BPM, not 87 with something in between: nothing tells the two halves apart.
        val random = Random(7)
        val result = track(Mix(20.0).beat(174.0, random, first = 0.2).noise(0.01, random).pcm(), 174.0, 0.2)
        assertTrue(result.worstTempo <= TEMPO_TOLERANCE)
    }

    @Test
    fun `a played beat with loose timing, a snare, hats and a bass line is still followed`() {
        for (bpm in listOf(87.0, 100.0, 126.0)) {
            val random = Random(8)
            val mix = Mix(30.0).noise(0.02, random)
            val period = 60 / bpm
            var t = 0.31
            var beat = 0
            while (t < 30) {
                fun loose() = 0.008 * (random.nextDouble() * 2 - 1) // a drummer within 8 ms
                if (beat % 2 == 0) {
                    mix.kick(t + loose(), 0.5 * (0.8 + 0.4 * random.nextDouble()), random)
                } else {
                    mix.hat(t + loose(), 0.5, random).kick(t + loose(), 0.15, random) // a snare, more or less
                }
                mix.hat(t + period / 2 + loose(), 0.12 * (0.5 + random.nextDouble()), random)
                if (beat % 4 == 3) mix.hat(t + period * 0.75 + loose(), 0.1, random)
                mix.tone(55.0 * (1 + (beat / 4 % 3) * 0.25), 0.15, t, minOf(30.0, t + period * 0.9))
                t += period
                beat++
            }
            val result = track(mix.pcm(), bpm, 0.31, to = 29.5)
            assertTrue("$bpm BPM: tempo off by ${result.worstTempo}", result.worstTempo <= TEMPO_TOLERANCE)
            assertTrue("$bpm BPM: phase off by ${result.worstPhase} ms", result.worstPhase <= LOOSE_PHASE_TOLERANCE_MS)
        }
    }

    @Test
    fun `a tempo change is followed`() {
        val random = Random(9)
        val pcm = Mix(40.0).noise(0.01, random)
            .beat(120.0, random, first = 0.2, to = 20.0)
            .beat(140.0, random, first = 20.1, from = 20.0)
            .pcm()
        val analyzer = AudioAnalyzer()
        track(pcm.copyOfRange(0, 20 * AudioAnalyzer.SAMPLE_RATE), 120.0, 0.2, analyzer = analyzer)

        var worstTempo = 0.0
        var worstPhase = 0.0
        var wrongTempoShown = 0.0
        analyzer.feed(pcm.copyOfRange(20 * AudioAnalyzer.SAMPLE_RATE, pcm.size)) { t ->
            // While it makes up its mind it may keep the old tempo or show none, but never a third one.
            if (analyzer.beatValid && abs(analyzer.bpm - 120) > 3 && abs(analyzer.bpm - 140) > 3) wrongTempoShown = analyzer.bpm.toDouble()
            if (t >= CONVERGENCE) {
                assertTrue("no beat ${t} s after the change", analyzer.beatValid)
                worstTempo = maxOf(worstTempo, abs(analyzer.bpm - 140.0))
                worstPhase = maxOf(worstPhase, abs(phaseError(analyzer, t, 140.0, 0.1)))
            }
        }
        assertEquals("a tempo that was never played", 0.0, wrongTempoShown, 0.0)
        assertTrue("tempo off by $worstTempo after the change", worstTempo <= TEMPO_TOLERANCE)
        assertTrue("phase off by $worstPhase ms after the change", worstPhase <= PHASE_TOLERANCE_MS)
    }

    @Test
    fun `when the music stops the beat flag drops, whatever the room sounds like`() {
        for (room in listOf(0.0, 0.001, 0.01, 0.05)) {
            val random = Random(10)
            val pcm = Mix(45.0).noise(room, random).beat(120.0, random, to = 20.0).pcm()
            val analyzer = AudioAnalyzer()
            var lastValid = 0.0
            analyzer.feed(pcm) { t ->
                if (analyzer.beatValid) lastValid = t
                if (!analyzer.beatValid) assertEquals(0f, analyzer.bpm)
            }
            assertTrue("with room noise $room the beat flag stayed up until $lastValid s", lastValid in 20.0..20.0 + CONVERGENCE)
        }
    }

    @Test
    fun `sound without a pulse never gets a tempo`() {
        fun assertNoBeat(name: String, pcm: ShortArray) {
            val analyzer = AudioAnalyzer()
            var confidence = 0f
            analyzer.feed(pcm) { t ->
                assertFalse("$name got a beat of ${analyzer.bpm} BPM at $t s", analyzer.beatValid)
                confidence = maxOf(confidence, analyzer.beatConfidence)
            }
            assertTrue("$name: confidence reached $confidence", confidence < BeatTracker.MIN_CONFIDENCE)
        }
        assertNoBeat("steady noise", Mix(40.0).noise(0.2, Random(11)).pcm())
        assertNoBeat("a steady tone", Mix(40.0).tone(220.0, 0.3).pcm())
        assertNoBeat("noise that swells and fades", Mix(40.0).noise(0.2, Random(12)).apply {
            for (i in data.indices) data[i] *= (0.55 + 0.45 * kotlin.math.sin(i / 16000.0 * 0.9)).toFloat()
        }.pcm())
        for (seed in 1..5) {
            // Hits at random moments, 0.15 to 1.05 s apart.
            val random = Random(seed)
            val mix = Mix(40.0).noise(0.005, random)
            var t = 0.3
            while (t < 40) {
                mix.kick(t, 0.5, random)
                t += 0.15 + random.nextDouble() * 0.9
            }
            assertNoBeat("random hits $seed", mix.pcm())
        }
        for (seed in 1..4) {
            // Something like speech: syllables of uneven length with uneven gaps.
            val random = Random(seed)
            val mix = Mix(40.0).noise(0.003, random)
            var t = 0.3
            while (t < 39) {
                val length = 0.08 + random.nextDouble() * 0.15
                mix.tone(180.0 + random.nextDouble() * 60, 0.2, t, t + length)
                    .tone(700.0 + random.nextDouble() * 900, 0.1, t, t + length)
                    .noise(0.03, random, t, t + length)
                t += length + 0.05 + random.nextDouble() * 0.25
                if (random.nextInt(6) == 0) t += 0.5
            }
            assertNoBeat("speech $seed", mix.pcm())
        }
    }

    // ---- mechanics --------------------------------------------------------------------------------------------------

    @Test
    fun `the size of the blocks it is fed in makes no difference`() {
        val random = Random(13)
        val pcm = Mix(12.0).beat(128.0, random, hats = 0.1).noise(0.01, random).pcm()
        fun run(block: Int): List<Any> {
            val analyzer = AudioAnalyzer()
            analyzer.feed(pcm, block)
            return listOf(analyzer.level, analyzer.bass, analyzer.mid, analyzer.treble, analyzer.bpm, analyzer.beatPhase, analyzer.onsets, analyzer.frames)
        }
        val reference = run(320)
        for (block in listOf(1, 7, 160, 333, 4096, pcm.size)) assertEquals("in blocks of $block", reference, run(block))
    }

    @Test
    fun `the phase is known for the last sample, not the last frame`() {
        // Feed a locked analyser one sample at a time: the phase must advance with every sample, evenly.
        val random = Random(14)
        val pcm = Mix(8.0).beat(120.0, random).pcm()
        val analyzer = AudioAnalyzer()
        analyzer.feed(pcm.copyOfRange(0, pcm.size - 400))
        assertTrue(analyzer.beatValid)
        var last = analyzer.beatPhase
        for (i in pcm.size - 400 until pcm.size) {
            analyzer.process(pcm, i, 1)
            var step = analyzer.beatPhase - last
            step -= floor(step + 0.5f)
            // One sample of a 0.5 s beat is 1/8000 of it; a frame boundary may correct by a little more.
            assertTrue("step $step at sample $i", step > 0f && step < 0.004f)
            last = analyzer.beatPhase
        }
    }

    @Test
    fun `reset forgets everything`() {
        val random = Random(15)
        val pcm = Mix(10.0).beat(100.0, random).pcm()
        val analyzer = AudioAnalyzer()
        analyzer.feed(pcm)
        assertTrue(analyzer.beatValid)
        analyzer.reset()
        assertFalse(analyzer.beatValid)
        assertEquals(0, analyzer.level)
        assertEquals(0L, analyzer.frames)
        analyzer.feed(Mix(1.0).pcm())
        assertEquals(0, analyzer.level)
        assertFalse(analyzer.beatValid)
    }

    @Test
    fun `analysing allocates nothing`() {
        val bean = ManagementFactory.getThreadMXBean()
        assumeTrue("this JVM does not count allocations per thread", bean is com.sun.management.ThreadMXBean)
        bean as com.sun.management.ThreadMXBean
        val random = Random(16)
        val pcm = Mix(30.0).beat(128.0, random, hats = 0.1).noise(0.02, random).pcm()
        val analyzer = AudioAnalyzer()
        val thread = Thread.currentThread().threadId()
        fun pass() {
            var offset = 0
            while (offset < pcm.size) {
                val count = minOf(320, pcm.size - offset)
                analyzer.process(pcm, offset, count)
                offset += count
            }
        }
        repeat(3) { pass() } // let the JIT finish with it
        val before = bean.getThreadAllocatedBytes(thread)
        repeat(4) { pass() } // two minutes of audio: 6000 blocks, 12000 frames, 480 tempo estimates
        val allocated = bean.getThreadAllocatedBytes(thread) - before
        assertTrue("$allocated bytes allocated while analysing two minutes of audio", allocated < 4096)
    }

    @Test
    fun `a second of audio costs a small fraction of a second`() {
        val random = Random(17)
        val pcm = Mix(60.0).beat(128.0, random, hats = 0.1).noise(0.02, random).pcm()
        val analyzer = AudioAnalyzer()
        repeat(2) { analyzer.feed(pcm) }
        val start = System.nanoTime()
        repeat(3) { analyzer.feed(pcm) }
        val millisPerSecond = (System.nanoTime() - start) / 1e6 / 180
        println("AudioAnalyzer: %.3f ms per second of audio on this JVM".format(millisPerSecond))
        // About 1 ms here. The bound is loose because build machines are busy; it catches an accidental O(n²).
        assertTrue("$millisPerSecond ms per second of audio", millisPerSecond < 25)
    }

    private companion object {
        /** Seconds from the first beat until tempo and phase are right, and from the last until the flag is off. */
        const val CONVERGENCE = 6.0
        const val TEMPO_TOLERANCE = 1.0
        const val PHASE_TOLERANCE_MS = 12.0

        /** For a beat played with 8 ms of looseness in every hit. */
        const val LOOSE_PHASE_TOLERANCE_MS = 20.0
    }
}
