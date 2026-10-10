package com.ouassimf.jacket.live

import kotlin.math.floor

/**
 * The arithmetic that carries a beat phase from the moment it was measured to the moment a frame is sent, and
 * applies the user's latency offset on the way.
 *
 * A phase is a position within the beat: 0 on the beat, growing to just under 1 right before the next one. The
 * analyser knows it for the last sample it heard, which was captured at some instant of the phone's monotonic
 * clock (the anchor). A frame leaves a little later, and reaches the LEDs later still. So the phase put in a
 * frame is the one the beat will have at `send time + offset`: with the offset equal to the time from sending
 * to light, the jacket flashes on the beat. No protocol field is needed for it; to the jacket the phase simply
 * is the phase at the frame's `t_ms`.
 */
object BeatClock {

    /**
     * The phase at [atNanos], given that it was [phase] at [anchorNanos] and a beat lasts [periodNanos].
     * [atNanos] may lie before the anchor.
     */
    fun phaseAt(phase: Float, anchorNanos: Long, periodNanos: Long, atNanos: Long): Double {
        require(periodNanos > 0) { "a beat must last some time" }
        val turns = phase + (atNanos - anchorNanos).toDouble() / periodNanos
        val wrapped = turns - floor(turns)
        return if (wrapped >= 1.0) 0.0 else wrapped // -1e-17 - floor(...) rounds to exactly 1.0
    }

    /** The phase to send in a frame built at [nowNanos], shifted [offsetMs] into the future (or past, if negative). */
    fun phaseToSend(phase: Float, anchorNanos: Long, periodNanos: Long, nowNanos: Long, offsetMs: Int): Double =
        phaseAt(phase, anchorNanos, periodNanos, nowNanos + offsetMs * NANOS_PER_MILLI)

    /** A phase as the protocol's `beat_phase`: 0 on the beat, 128 half-way, 255 just before the next. */
    fun toByte(phase: Double): Int = (phase * 256).toInt().coerceIn(0, 255)

    const val NANOS_PER_MILLI = 1_000_000L
}
