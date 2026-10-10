package com.ouassimf.jacket.live

import com.ouassimf.jacket.protocol.LiveFrame
import kotlin.math.roundToInt

/**
 * What live mode knows right now: the values of the next frame, and the same for the screen's meters. One
 * instance is refilled thirty times a second; nothing here is observable state on purpose, the screen redraws
 * from it when told to.
 */
class LiveMeters {
    var audioValid = false
    var beatValid = false
    var motionValid = false
    var level = 0
    var bass = 0
    var mid = 0
    var treble = 0
    var bpmX10 = 0

    /** The beat position that is sent: the heard one moved by the latency offset. 0 to 255. */
    var beatPhase = 0

    /** The beat position as the phone hears it right now, 0 until 1: what an on-screen beat light follows. */
    var heardPhase = 0f
    var motion = 0
    var tiltX = 0
    var tiltY = 0

    fun clear() {
        audioValid = false
        beatValid = false
        motionValid = false
        level = 0; bass = 0; mid = 0; treble = 0
        bpmX10 = 0
        beatPhase = 0
        heardPhase = 0f
        motion = 0; tiltX = 0; tiltY = 0
    }
}

/**
 * Makes live frames out of the analysers' latest results.
 *
 * The rules a frame follows, which the jacket can rely on:
 * - a flag is set only while its source is switched on and its results are fresh ([STALE_NANOS]); the fields
 *   belonging to a clear flag are 0;
 * - `beat_phase` is where the beat will be at the frame's own `t_ms` plus the latency offset, carried forward
 *   from the instant the analyser measured it;
 * - `seq` counts frames built for sending, wrapping at 256; `t_ms` is the low 16 bits of the monotonic clock in
 *   milliseconds at the moment the frame was built.
 */
class LiveFrameBuilder {
    private val snapshot = LiveFeatures.Snapshot()
    private var seq = 0

    /** Fills [into] with what a frame built at [nowNanos] would carry. */
    fun sample(features: LiveFeatures, settings: LiveSettings, nowNanos: Long, into: LiveMeters) {
        features.read(snapshot)
        val s = snapshot

        into.audioValid = settings.sound && s.hasAudio && nowNanos - s.audioAtNanos <= STALE_NANOS
        into.level = if (into.audioValid) s.level.coerceIn(0, 255) else 0
        into.bass = if (into.audioValid) s.bass.coerceIn(0, 255) else 0
        into.mid = if (into.audioValid) s.mid.coerceIn(0, 255) else 0
        into.treble = if (into.audioValid) s.treble.coerceIn(0, 255) else 0

        into.beatValid = into.audioValid && s.beatValid && s.beatPeriodNanos > 0
        if (into.beatValid) {
            into.bpmX10 = (s.bpm * 10).roundToInt().coerceIn(0, 0xFFFF)
            into.beatPhase = BeatClock.toByte(BeatClock.phaseToSend(s.beatPhase, s.audioAtNanos, s.beatPeriodNanos, nowNanos, settings.latencyOffsetMs))
            into.heardPhase = BeatClock.phaseAt(s.beatPhase, s.audioAtNanos, s.beatPeriodNanos, nowNanos).toFloat()
        } else {
            into.bpmX10 = 0
            into.beatPhase = 0
            into.heardPhase = 0f
        }

        into.motionValid = settings.motion && s.hasMotion && nowNanos - s.motionAtNanos <= STALE_NANOS
        into.motion = if (into.motionValid) s.motion.coerceIn(0, 255) else 0
        into.tiltX = if (into.motionValid) s.tiltX.coerceIn(-127, 127) else 0
        into.tiltY = if (into.motionValid) s.tiltY.coerceIn(-127, 127) else 0
    }

    /** The next frame, carrying [meters] as sampled at [nowNanos]. Every call uses up one `seq`. */
    fun frame(meters: LiveMeters, nowNanos: Long): LiveFrame {
        val flags = (if (meters.audioValid) LiveFrame.FLAG_AUDIO else 0) or
            (if (meters.motionValid) LiveFrame.FLAG_MOTION else 0) or
            (if (meters.beatValid) LiveFrame.FLAG_BEAT else 0)
        val frame = LiveFrame(
            flags = flags,
            seq = seq,
            level = meters.level,
            bass = meters.bass,
            mid = meters.mid,
            treble = meters.treble,
            bpmX10 = meters.bpmX10,
            beatPhase = meters.beatPhase,
            motion = meters.motion,
            tiltX = meters.tiltX,
            tiltY = meters.tiltY,
            tMs = (Math.floorDiv(nowNanos, BeatClock.NANOS_PER_MILLI) and 0xFFFF).toInt(),
        )
        seq = (seq + 1) and 0xFF
        return frame
    }

    companion object {
        /** Results older than this are not sent: the microphone or the sensor has stalled. */
        const val STALE_NANOS = 400 * BeatClock.NANOS_PER_MILLI
    }
}
