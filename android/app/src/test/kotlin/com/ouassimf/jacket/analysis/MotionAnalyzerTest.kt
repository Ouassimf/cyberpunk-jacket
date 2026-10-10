package com.ouassimf.jacket.analysis

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import kotlin.random.Random

class MotionAnalyzerTest {

    /** Feeds accelerometer samples at [rate] Hz and keeps the clock, like a sensor would. */
    private class Phone(val analyzer: MotionAnalyzer = MotionAnalyzer(), val rate: Int = 50, seed: Int = 1) {
        private val random = Random(seed)
        var nanos = 5_000_000_000L
        val seconds: Double get() = (nanos - 5_000_000_000L) / 1e9

        /** [seconds] of samples: gravity ([gx], [gy], [gz]) plus [shake] m/s² of sine at [hz] along x, plus sensor noise. */
        fun hold(seconds: Double, gx: Double = 0.0, gy: Double = 0.0, gz: Double = G, shake: Double = 0.0, hz: Double = 3.0, each: () -> Unit = {}) {
            repeat((seconds * rate).toInt()) {
                val t = nanos / 1e9
                val wobble = shake * sin(2 * PI * hz * t)
                fun noise() = (random.nextDouble() - 0.5) * 0.06
                analyzer.add((gx + wobble + noise()).toFloat(), (gy + noise()).toFloat(), (gz + noise()).toFloat(), nanos)
                nanos += 1_000_000_000L / rate
                each()
            }
        }
    }

    @Test
    fun `a phone lying still reads no motion and no tilt`() {
        val phone = Phone()
        phone.hold(10.0) {
            assertEquals(0, phone.analyzer.motion)
            assertTrue(abs(phone.analyzer.tiltX) <= 1 && abs(phone.analyzer.tiltY) <= 1)
        }
    }

    @Test
    fun `a phone held still in any position reads no motion`() {
        for ((gx, gy, gz) in listOf(Triple(G, 0.0, 0.0), Triple(0.0, -G, 0.0), Triple(5.0, 5.0, 6.8), Triple(0.0, 0.0, -G))) {
            val phone = Phone()
            phone.hold(5.0, gx, gy, gz)
            assertEquals("gravity ($gx, $gy, $gz) is not movement", 0, phone.analyzer.motion)
        }
    }

    @Test
    fun `shaking reads high within a moment and falls back when it ends`() {
        val phone = Phone()
        phone.hold(2.0)
        var highest = 0
        phone.hold(1.0, shake = 15.0, hz = 4.0) { highest = maxOf(highest, phone.analyzer.motion) }
        assertTrue("shaken: $highest", highest >= 230)
        phone.hold(0.4)
        // The jacket smooths motion itself (about 400 ms down): what is sent must already be gone by then.
        assertTrue("0.4 s after the shaking stopped: ${phone.analyzer.motion}", phone.analyzer.motion <= 15)
        phone.hold(3.0)
        assertEquals(0, phone.analyzer.motion)
    }

    @Test
    fun `walking registers, and so does dancing after it`() {
        val phone = Phone()
        phone.hold(2.0)
        val walking = ArrayList<Int>()
        phone.hold(6.0, shake = 3.5, hz = 2.0) { if (phone.seconds > 4.0) walking += phone.analyzer.motion }
        assertTrue("walking peaks at ${walking.max()}", walking.max() in 120..255)
        assertTrue("walking averages ${walking.average()}", walking.average() >= 70)

        val dancing = ArrayList<Int>()
        phone.hold(10.0, shake = 14.0, hz = 2.5) { if (phone.seconds > 14.0) dancing += phone.analyzer.motion }
        assertTrue("dancing peaks at ${dancing.max()}", dancing.max() >= 220)
        assertTrue("it found its range: averages ${dancing.average()}, not pinned at the top", dancing.average() in 100.0..245.0)

        // Straight after dancing a walk reads small, and then comes back as the range closes in again.
        val after = ArrayList<Int>()
        phone.hold(2.0, shake = 3.5, hz = 2.0) { after += phone.analyzer.motion }
        assertTrue("walking right after dancing averages ${after.takeLast(50).average()}", after.takeLast(50).average() < walking.average() * 0.6)
        val later = ArrayList<Int>()
        phone.hold(12.0, shake = 3.5, hz = 2.0) { if (phone.seconds > 28.0) later += phone.analyzer.motion }
        assertTrue("and ${later.average()} ten seconds on", later.average() >= walking.average() * 0.9)
    }

    @Test
    fun `tilt is the direction of gravity, with the right signs`() {
        fun tilt(gx: Double, gy: Double, gz: Double): Pair<Int, Int> {
            val phone = Phone()
            phone.hold(4.0, gx, gy, gz)
            return phone.analyzer.tiltX to phone.analyzer.tiltY
        }
        assertEquals("flat on the table", 0 to 0, tilt(0.0, 0.0, G))
        assertEquals("face down", 0 to 0, tilt(0.0, 0.0, -G))
        assertEquals("upright", 0 to 127, tilt(0.0, G, 0.0))
        assertEquals("upside down", 0 to -127, tilt(0.0, -G, 0.0))
        assertEquals("on its left edge, right edge up", 127 to 0, tilt(G, 0.0, 0.0))
        assertEquals("on its right edge", -127 to 0, tilt(-G, 0.0, 0.0))
        val (x, y) = tilt(G * sin(PI / 6), 0.0, G * cos(PI / 6))
        assertTrue("30 degrees is half: $x", x in 62..65)
        assertEquals(0, y)
        val (x2, y2) = tilt(-G * 0.5, G * 0.5, G * 0.7071)
        assertTrue("both at once: $x2, $y2", x2 in -65..-62 && y2 in 62..65)
    }

    @Test
    fun `tilt never leaves its range, whatever the sensor says`() {
        val phone = Phone()
        phone.hold(3.0, gx = 40.0, gz = 0.0) // being swung round: far more than gravity
        assertEquals(127, phone.analyzer.tiltX)
        val random = Random(5)
        repeat(2000) {
            phone.analyzer.add(random.nextFloat() * 160 - 80, random.nextFloat() * 160 - 80, random.nextFloat() * 160 - 80, phone.nanos)
            phone.nanos += 20_000_000
            assertTrue(phone.analyzer.tiltX in -127..127 && phone.analyzer.tiltY in -127..127)
            assertTrue(phone.analyzer.motion in 0..255)
        }
    }

    @Test
    fun `tilt moves smoothly to a new position`() {
        val phone = Phone()
        phone.hold(3.0)
        val seen = ArrayList<Int>()
        phone.hold(3.0, gx = G, gz = 0.0) { seen += phone.analyzer.tiltX }
        assertTrue("after 0.1 s: ${seen[4]}", seen[4] in 15..50)
        assertTrue("after 0.4 s: ${seen[19]}", seen[19] in 70..110)
        assertTrue("after 2 s: ${seen[99]}", seen[99] >= 125)
        assertEquals("it only ever rises", seen.sorted(), seen)
    }

    @Test
    fun `in free fall the last tilt is kept`() {
        val phone = Phone()
        phone.hold(3.0, gx = G, gz = 0.0)
        phone.hold(5.0, gx = 0.0, gz = 0.0)
        assertEquals(127, phone.analyzer.tiltX)
    }

    @Test
    fun `the sample rate does not change the result`() {
        fun run(rate: Int): Triple<Int, Int, Int> {
            val phone = Phone(rate = rate)
            phone.hold(2.0, gx = 3.0, gy = 2.0, gz = 9.1)
            var highest = 0
            phone.hold(3.0, gx = 3.0, gy = 2.0, gz = 9.1, shake = 6.0, hz = 2.0) { highest = maxOf(highest, phone.analyzer.motion) }
            return Triple(highest, phone.analyzer.tiltX, phone.analyzer.tiltY)
        }
        val slow = run(50)
        val fast = run(200)
        assertTrue("$slow at 50 Hz, $fast at 200 Hz", abs(slow.first - fast.first) <= 12)
        assertTrue(abs(slow.second - fast.second) <= 3 && abs(slow.third - fast.third) <= 3)
    }

    @Test
    fun `a gap in the samples does no harm`() {
        val phone = Phone()
        phone.hold(2.0)
        phone.nanos += 60_000_000_000L // the sensor paused for a minute
        phone.hold(0.1)
        assertEquals(0, phone.analyzer.motion)
        phone.nanos -= 120_000_000_000L // and a clock that steps back
        phone.hold(1.0)
        assertEquals(0, phone.analyzer.motion)
    }

    @Test
    fun `sensitivity makes small movements count for more`() {
        fun peak(sensitivity: Int): Int {
            val phone = Phone()
            phone.analyzer.sensitivity = sensitivity
            phone.hold(2.0)
            var highest = 0
            phone.hold(3.0, shake = 1.5, hz = 2.0) { highest = maxOf(highest, phone.analyzer.motion) }
            return highest
        }
        val low = peak(0)
        val neutral = peak(50)
        val high = peak(100)
        assertTrue("$low < $neutral < $high", low < neutral && neutral < high)
        assertTrue("about double and half: $low, $neutral, $high", abs(high - 2 * neutral) <= 8 && abs(neutral - 2 * low) <= 8)
    }

    @Test
    fun `reset starts over`() {
        val phone = Phone()
        phone.hold(2.0, gx = G, gz = 0.0, shake = 10.0)
        phone.analyzer.reset()
        assertEquals(0, phone.analyzer.motion)
        assertEquals(0, phone.analyzer.tiltX)
        phone.hold(0.1)
        assertEquals("the first sample after a reset is taken as gravity", 0, phone.analyzer.tiltX)
        assertEquals(0, phone.analyzer.motion)
    }

    private companion object {
        const val G = 9.80665
    }
}
