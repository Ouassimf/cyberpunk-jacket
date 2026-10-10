package com.ouassimf.jacket.data

import com.ouassimf.jacket.data.LayoutProblem.Kind
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

class ZonesTest {

    private val strips = listOf(144, 144)

    private fun config(effect: Int) = EffectConfig(effect, 0, 128, 128, Rgb(0x00FFCC), Rgb(0xFF00FF))
    private fun zone(mask: Int, start: Int, length: Int, flags: Int = 0, effect: Int = 0) =
        Segment(SegmentGeometry(mask, start, length, flags), config(effect))

    private fun problems(vararg geometry: SegmentGeometry, strips: List<Int> = this.strips, max: Int = 8) =
        Zones.problems(geometry.toList(), strips, max)

    // ---- validation -------------------------------------------------------------------------------------------------

    @Test
    fun `layouts the jacket accepts have no problems`() {
        assertEquals(emptyList<LayoutProblem>(), problems(SegmentGeometry(3, 0, 144, 0)))
        assertEquals(emptyList<LayoutProblem>(), problems(SegmentGeometry(1, 0, 144, 1), SegmentGeometry(2, 143, 1, 0)))
        assertEquals("overlaps are allowed", emptyList<LayoutProblem>(), problems(SegmentGeometry(3, 0, 100, 0), SegmentGeometry(1, 50, 94, 0)))
        assertEquals(emptyList<LayoutProblem>(), Zones.problems(List(8) { SegmentGeometry(3, it, 1, 0) }, strips))
    }

    @Test
    fun `each way of being wrong is named, with the zone it is about`() {
        assertEquals(listOf(LayoutProblem(Kind.NoSegments)), problems())
        assertEquals(listOf(LayoutProblem(Kind.TooManySegments)), Zones.problems(List(9) { SegmentGeometry(3, 0, 10, 0) }, strips))
        assertEquals(listOf(LayoutProblem(Kind.TooManySegments)), problems(SegmentGeometry(3, 0, 1, 0), SegmentGeometry(3, 1, 1, 0), max = 1))
        assertEquals(listOf(LayoutProblem(Kind.NoStrip, 0)), problems(SegmentGeometry(0, 0, 10, 0)))
        assertEquals(listOf(LayoutProblem(Kind.UnknownStrip, 1)), problems(SegmentGeometry(1, 0, 10, 0), SegmentGeometry(4, 0, 10, 0)))
        assertEquals(listOf(LayoutProblem(Kind.Empty, 0)), problems(SegmentGeometry(1, 5, 0, 0)))
        assertEquals(listOf(LayoutProblem(Kind.OutsideStrip, 0)), problems(SegmentGeometry(1, 1, 144, 0)))
        assertEquals(listOf(LayoutProblem(Kind.OutsideStrip, 0)), problems(SegmentGeometry(1, 144, 1, 0)))
        assertEquals(listOf(LayoutProblem(Kind.UnknownFlags, 0)), problems(SegmentGeometry(1, 0, 10, 2)))
        assertEquals(
            "every problem is listed, not only the first",
            listOf(LayoutProblem(Kind.Empty, 0), LayoutProblem(Kind.OutsideStrip, 2), LayoutProblem(Kind.UnknownFlags, 2)),
            problems(SegmentGeometry(3, 0, 0, 0), SegmentGeometry(3, 0, 1, 0), SegmentGeometry(2, 100, 45, 0x80)),
        )
    }

    @Test
    fun `a zone on two strips of different length must fit the shorter one`() {
        val uneven = listOf(144, 60)
        assertEquals(60, Zones.stripLength(3, uneven))
        assertEquals(144, Zones.stripLength(1, uneven))
        assertEquals(0, Zones.stripLength(0, uneven))
        assertEquals(emptyList<LayoutProblem>(), problems(SegmentGeometry(1, 0, 144, 0), SegmentGeometry(3, 0, 60, 0), strips = uneven))
        assertEquals(listOf(LayoutProblem(Kind.OutsideStrip, 0)), problems(SegmentGeometry(3, 0, 61, 0), strips = uneven))
        assertEquals(listOf(LayoutProblem(Kind.UnknownStrip, 0)), problems(SegmentGeometry(2, 0, 10, 0), strips = listOf(144)))
    }

    // ---- ready-made layouts -----------------------------------------------------------------------------------------

    @Test
    fun `mirrored is one zone on both strips playing what zone one played`() {
        val current = listOf(zone(1, 0, 144, effect = 4), zone(2, 0, 144, effect = 5))
        assertEquals(listOf(zone(3, 0, 144, effect = 4)), Zones.mirrored(current, strips))
        assertEquals(listOf(zone(3, 0, 60, effect = 4)), Zones.mirrored(current, listOf(144, 60)))
    }

    @Test
    fun `per strip is one zone on each strip, keeping the effects that were there`() {
        assertEquals(
            listOf(zone(1, 0, 144, effect = 4), zone(2, 0, 144, effect = 4)),
            Zones.perStrip(listOf(zone(3, 0, 144, effect = 4)), strips),
        )
        assertEquals(
            listOf(zone(1, 0, 144, effect = 4), zone(2, 0, 60, effect = 5)),
            Zones.perStrip(listOf(zone(3, 0, 10, effect = 4), zone(3, 10, 10, effect = 5), zone(3, 20, 10, effect = 6)), listOf(144, 60)),
        )
    }

    // ---- editing ------------------------------------------------------------------------------------------------------

    @Test
    fun `the ends of a zone move one LED at a time and stop at the other end and at the strip`() {
        val zones = listOf(zone(1, 20, 30))
        fun start(pixel: Int) = Zones.moveStart(zones, 0, pixel)[0].geometry.let { it.start to it.end }
        fun end(pixel: Int) = Zones.moveEnd(zones, 0, pixel, strips)[0].geometry.let { it.start to it.end }

        assertEquals(10 to 50, start(10))
        assertEquals(0 to 50, start(-5))
        assertEquals("at least one LED remains", 49 to 50, start(50))
        assertEquals(49 to 50, start(400))

        assertEquals(20 to 60, end(60))
        assertEquals(20 to 144, end(144))
        assertEquals(20 to 144, end(999))
        assertEquals("at least one LED remains", 20 to 21, end(20))
        assertEquals(20 to 21, end(-3))
        assertEquals("the shorter strip limits a zone on both", 30, Zones.moveEnd(listOf(zone(3, 0, 10)), 0, 99, listOf(144, 30))[0].geometry.end)
    }

    @Test
    fun `editing one zone leaves the others and every effect alone`() {
        val zones = listOf(zone(1, 0, 10, effect = 1), zone(1, 10, 10, effect = 2), zone(2, 0, 10, effect = 3))
        val moved = Zones.moveEnd(zones, 1, 40, strips)
        assertEquals(zones[0], moved[0])
        assertEquals(zones[2], moved[2])
        assertEquals(zones[1].config, moved[1].config)
        assertEquals(zones, Zones.moveEnd(zones, 7, 40, strips))
    }

    @Test
    fun `flipping toggles the reversed flag only`() {
        val zones = listOf(zone(3, 5, 10))
        val flipped = Zones.flip(zones, 0)
        assertTrue(flipped[0].geometry.reversed)
        assertEquals(zones, Zones.flip(flipped, 0))
    }

    @Test
    fun `changing strips pulls the zone back onto a shorter strip`() {
        val uneven = listOf(144, 60)
        val zones = listOf(zone(1, 100, 40))
        assertEquals(SegmentGeometry(2, 59, 1, 0), Zones.setStrips(zones, 0, 2, uneven)[0].geometry)
        assertEquals(SegmentGeometry(3, 20, 40, 0), Zones.setStrips(listOf(zone(1, 20, 100)), 0, 3, uneven)[0].geometry)
        assertEquals(SegmentGeometry(2, 100, 40, 0), Zones.setStrips(zones, 0, 2, strips)[0].geometry)
        assertSame("a zone must be on some strip", zones, Zones.setStrips(zones, 0, 0, uneven))
        assertSame(zones, Zones.setStrips(zones, 0, 4, uneven))
    }

    @Test
    fun `a new zone takes the largest free stretch of the selected zone's strips`() {
        val zones = listOf(zone(1, 0, 20, effect = 4), zone(1, 50, 10, effect = 5))
        val added = Zones.add(zones, like = 1, strips = strips)
        assertEquals(3, added.size)
        assertEquals("60..143 is longer than 20..49", SegmentGeometry(1, 60, 84, 0), added[2].geometry)
        assertEquals("it starts with zone one's effect, as the jacket does", zones[0].config, added[2].config)

        // Zones on the left strip take no room on the right one.
        val mixed = listOf(zone(1, 0, 144), zone(2, 10, 5))
        assertEquals("free on the right: 0..9 and 15..143", SegmentGeometry(2, 15, 129, 0), Zones.add(mixed, like = 1, strips = strips)[2].geometry)

        // A zone on both strips needs room on both.
        val both = listOf(zone(3, 0, 20), zone(1, 20, 10), zone(2, 30, 30))
        assertEquals(SegmentGeometry(3, 60, 84, 0), Zones.add(both, like = 0, strips = strips)[3].geometry)
    }

    @Test
    fun `with no free stretch a new zone sits on top of the second half of the selected one`() {
        val zones = listOf(zone(3, 0, 144))
        assertEquals(SegmentGeometry(3, 72, 72, 0), Zones.add(zones, 0, strips)[1].geometry)
        assertEquals(SegmentGeometry(1, 143, 1, 0), Zones.add(listOf(zone(3, 0, 143), zone(1, 143, 1)), 1, strips)[2].geometry)
    }

    @Test
    fun `no more zones than the jacket holds, and never fewer than one`() {
        val full = List(8) { zone(3, it * 10, 10) }
        assertSame(full, Zones.add(full, 0, strips))
        val two = full.take(2)
        assertSame(two, Zones.add(two, 0, strips, maxSegments = 2))
        assertEquals(3, Zones.add(two, 0, strips, maxSegments = 3).size)

        val one = listOf(zone(3, 0, 144))
        assertSame(one, Zones.remove(one, 0))
        assertSame(full, Zones.remove(full, 8))
        assertEquals(full.filterIndexed { i, _ -> i != 3 }, Zones.remove(full, 3))
    }

    @Test
    fun `no sequence of edits can produce a layout the jacket would refuse`() {
        val random = Random(42)
        for (strips in listOf(listOf(144, 144), listOf(144, 60), listOf(10, 144))) {
            var zones = listOf(zone(3, 0, Zones.stripLength(3, strips)))
            repeat(5_000) { step ->
                val index = random.nextInt(-1, zones.size + 1)
                zones = when (random.nextInt(9)) {
                    0 -> Zones.moveStart(zones, index, random.nextInt(-50, 300))
                    1 -> Zones.moveEnd(zones, index, random.nextInt(-50, 300), strips)
                    2 -> Zones.setStrips(zones, index, random.nextInt(0, 8), strips)
                    3 -> Zones.flip(zones, index)
                    4 -> Zones.add(zones, index, strips)
                    5 -> Zones.remove(zones, index)
                    6 -> Zones.mirrored(zones, strips)
                    7 -> Zones.perStrip(zones, strips)
                    else -> Zones.add(zones, index, strips, maxSegments = 3)
                }
                val problems = Zones.problems(zones.map { it.geometry }, strips)
                assertEquals("step $step with strips $strips left $zones", emptyList<LayoutProblem>(), problems)
            }
            assertFalse(zones.isEmpty())
        }
    }
}
