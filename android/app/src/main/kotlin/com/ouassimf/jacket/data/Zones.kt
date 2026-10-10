package com.ouassimf.jacket.data

import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry

/** One reason the jacket would refuse a layout. [segment] is the offending index, null when it is the whole layout. */
data class LayoutProblem(val kind: Kind, val segment: Int? = null) {
    enum class Kind { NoSegments, TooManySegments, NoStrip, UnknownStrip, Empty, OutsideStrip, UnknownFlags }
}

/**
 * Editing a segment layout ("zones"), as pure functions over the list of segments: the zones screen keeps a draft,
 * changes it through these, and sends it once with [JacketRepository.applyLayout] when the user applies it.
 *
 * [strips] is always the LED count of each strip, as INFO gives it; bit *i* of a `strip_mask` is strip *i*.
 * Every function that edits returns a layout that is still valid: positions snap to whole pixels and are clamped
 * to the strip, so the editor cannot be dragged into a state the jacket would reject.
 */
object Zones {

    /** Everything wrong with [segments]; empty means the jacket will take it. Overlaps are fine: the higher index wins. */
    fun problems(segments: List<SegmentGeometry>, strips: List<Int>, maxSegments: Int = Protocol.MAX_SEGMENTS): List<LayoutProblem> =
        buildList {
            if (segments.isEmpty()) add(LayoutProblem(LayoutProblem.Kind.NoSegments))
            if (segments.size > maxSegments) add(LayoutProblem(LayoutProblem.Kind.TooManySegments))
            segments.forEachIndexed { i, g ->
                when {
                    g.stripMask == 0 -> add(LayoutProblem(LayoutProblem.Kind.NoStrip, i))
                    g.stripMask and allStrips(strips).inv() != 0 -> add(LayoutProblem(LayoutProblem.Kind.UnknownStrip, i))
                    g.length < 1 -> add(LayoutProblem(LayoutProblem.Kind.Empty, i))
                    g.start < 0 || g.end > stripLength(g.stripMask, strips) -> add(LayoutProblem(LayoutProblem.Kind.OutsideStrip, i))
                }
                if (g.flags and Protocol.GEOMETRY_REVERSED.inv() != 0) add(LayoutProblem(LayoutProblem.Kind.UnknownFlags, i))
            }
        }

    fun allStrips(strips: List<Int>): Int = (1 shl strips.size) - 1

    /** How many pixels a segment on the strips of [mask] can span: the shortest of them. 0 when [mask] names none. */
    fun stripLength(mask: Int, strips: List<Int>): Int =
        strips.filterIndexed { i, _ -> mask and (1 shl i) != 0 }.minOrNull() ?: 0

    // ---- ready-made layouts ---------------------------------------------------------------------------------------

    /** One segment showing the same pixels on every strip. It plays what segment 0 of [current] plays. */
    fun mirrored(current: List<Segment>, strips: List<Int>): List<Segment> {
        val mask = allStrips(strips)
        return listOf(Segment(SegmentGeometry(mask, 0, stripLength(mask, strips), 0), current[0].config))
    }

    /** One segment per strip, each on its own. Segment *i* keeps its effect; a new one starts with segment 0's. */
    fun perStrip(current: List<Segment>, strips: List<Int>): List<Segment> =
        strips.mapIndexed { i, length ->
            Segment(SegmentGeometry(1 shl i, 0, length, 0), (current.getOrNull(i) ?: current[0]).config)
        }

    // ---- editing ----------------------------------------------------------------------------------------------------

    /**
     * Appends a segment on the strips of segment [like], in the largest stretch nothing covers yet; if there is
     * none, over the second half of [like], where it shows on top. Returns [segments] unchanged when full.
     */
    fun add(segments: List<Segment>, like: Int, strips: List<Int>, maxSegments: Int = Protocol.MAX_SEGMENTS): List<Segment> {
        if (segments.size >= maxSegments) return segments
        val model = segments.getOrNull(like) ?: segments[0]
        val mask = model.geometry.stripMask
        val length = stripLength(mask, strips)
        val covered = BooleanArray(length)
        for (segment in segments) {
            if (segment.geometry.stripMask and mask == 0) continue
            for (pixel in segment.geometry.start until minOf(segment.geometry.end, length)) covered[pixel] = true
        }
        var bestStart = 0
        var bestLength = 0
        var runStart = 0
        for (pixel in 0..length) {
            if (pixel == length || covered[pixel]) {
                if (pixel - runStart > bestLength) {
                    bestStart = runStart
                    bestLength = pixel - runStart
                }
                runStart = pixel + 1
            }
        }
        if (bestLength == 0) {
            bestLength = maxOf(1, model.geometry.length / 2)
            bestStart = model.geometry.end - bestLength
        }
        return segments + Segment(SegmentGeometry(mask, bestStart, bestLength, 0), segments[0].config)
    }

    /** Removes segment [index]; the others keep their effects. The last segment cannot be removed. */
    fun remove(segments: List<Segment>, index: Int): List<Segment> =
        if (segments.size <= 1 || index !in segments.indices) segments else segments.filterIndexed { i, _ -> i != index }

    /** Drags the first pixel of segment [index] to [pixel], its last pixel staying where it is. */
    fun moveStart(segments: List<Segment>, index: Int, pixel: Int): List<Segment> = edit(segments, index) { g ->
        val start = pixel.coerceIn(0, g.end - 1)
        g.copy(start = start, length = g.end - start)
    }

    /** Drags the end of segment [index] (one past its last pixel) to [pixel], its first pixel staying where it is. */
    fun moveEnd(segments: List<Segment>, index: Int, pixel: Int, strips: List<Int>): List<Segment> = edit(segments, index) { g ->
        g.copy(length = pixel.coerceIn(g.start + 1, stripLength(g.stripMask, strips)) - g.start)
    }

    /** Puts segment [index] on the strips of [mask], pulled back onto them if they are shorter. No-op for an empty mask. */
    fun setStrips(segments: List<Segment>, index: Int, mask: Int, strips: List<Int>): List<Segment> {
        val limit = stripLength(mask and allStrips(strips), strips)
        if (limit == 0) return segments
        return edit(segments, index) { g ->
            val start = g.start.coerceAtMost(limit - 1)
            g.copy(stripMask = mask and allStrips(strips), start = start, length = g.length.coerceAtMost(limit - start))
        }
    }

    /** Turns segment [index] around: its effect then runs from the other end. */
    fun flip(segments: List<Segment>, index: Int): List<Segment> =
        edit(segments, index) { it.copy(flags = it.flags xor Protocol.GEOMETRY_REVERSED) }

    private inline fun edit(segments: List<Segment>, index: Int, change: (SegmentGeometry) -> SegmentGeometry): List<Segment> =
        segments.mapIndexed { i, segment -> if (i == index) segment.copy(geometry = change(segment.geometry)) else segment }
}

/** Which segment Home's controls write to ([edited], possibly "all") and whose settings they show ([shown]). */
data class SegmentTarget(val edited: Int, val shown: Int) {
    companion object {
        /**
         * [selected] is the user's pick, null for "All". With a single segment, or a pick that no longer exists
         * (the layout shrank), it is "All": every segment is written, and segment 0 is the one on show.
         */
        fun of(selected: Int?, segmentCount: Int): SegmentTarget =
            if (selected == null || segmentCount <= 1 || selected !in 0 until segmentCount) {
                SegmentTarget(Protocol.ALL_SEGMENTS, 0)
            } else {
                SegmentTarget(selected, selected)
            }
    }
}
