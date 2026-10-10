package com.ouassimf.jacket.live

/**
 * Where the analysers leave their latest results and the streaming side picks them up. The microphone thread
 * and the sensor thread write, the main thread reads thirty times a second; nobody waits for anybody beyond
 * the copy of a dozen numbers, so audio capture is never held up by the main thread.
 *
 * Every result carries the instant it is true for, on one monotonic clock (the one the controller is given):
 * results that are too old are not sent, and the beat phase is carried forward from that instant, see
 * [BeatClock].
 *
 * The analysers produce results faster than frames are sent. For the levels and the motion a frame therefore
 * gets the highest value since the frame before, not the last: a drum hit that fell between two frames is still
 * sent at full height, with no smoothing and no delay. This makes [read] the business of one reader only.
 */
class LiveFeatures {
    private val lock = Any()
    private val current = Snapshot()
    private var peakLevel = 0
    private var peakBass = 0
    private var peakMid = 0
    private var peakTreble = 0
    private var peakMotion = 0
    private var audioSinceRead = false
    private var motionSinceRead = false

    /**
     * The latest audio analysis. [atNanos] is when the last analysed sample was captured, which is also the
     * instant [beatPhase] (0 until 1) belongs to. [beatPeriodSeconds] and [bpm] only count when [beatValid].
     */
    fun audio(atNanos: Long, level: Int, bass: Int, mid: Int, treble: Int, beatValid: Boolean, bpm: Float, beatPhase: Float, beatPeriodSeconds: Float) {
        synchronized(lock) {
            current.hasAudio = true
            current.audioAtNanos = atNanos
            current.level = level
            current.bass = bass
            current.mid = mid
            current.treble = treble
            if (!audioSinceRead || level > peakLevel) peakLevel = level
            if (!audioSinceRead || bass > peakBass) peakBass = bass
            if (!audioSinceRead || mid > peakMid) peakMid = mid
            if (!audioSinceRead || treble > peakTreble) peakTreble = treble
            audioSinceRead = true
            current.beatValid = beatValid && beatPeriodSeconds > 0f
            current.bpm = bpm
            current.beatPhase = beatPhase
            current.beatPeriodNanos = (beatPeriodSeconds * 1e9).toLong()
        }
    }

    /** The latest motion analysis, with the instant it was made. */
    fun motion(atNanos: Long, motion: Int, tiltX: Int, tiltY: Int) {
        synchronized(lock) {
            current.hasMotion = true
            current.motionAtNanos = atNanos
            current.motion = motion
            if (!motionSinceRead || motion > peakMotion) peakMotion = motion
            motionSinceRead = true
            current.tiltX = tiltX
            current.tiltY = tiltY
        }
    }

    /** The microphone stopped: what it last heard must not be sent any more. */
    fun clearAudio() {
        synchronized(lock) {
            current.hasAudio = false
            current.beatValid = false
        }
    }

    fun clearMotion() {
        synchronized(lock) { current.hasMotion = false }
    }

    /**
     * Copies the latest results into [into], all from the same moment, with the levels and the motion at their
     * highest since the last call.
     */
    fun read(into: Snapshot) {
        synchronized(lock) {
            into.copyFrom(current) // with nothing new since the last call, that is where things stand
            if (audioSinceRead) {
                into.level = peakLevel
                into.bass = peakBass
                into.mid = peakMid
                into.treble = peakTreble
            }
            if (motionSinceRead) into.motion = peakMotion
            audioSinceRead = false
            motionSinceRead = false
        }
    }

    /** One consistent set of results. Mutable so that reading allocates nothing. */
    class Snapshot {
        var hasAudio = false
        var audioAtNanos = 0L
        var level = 0
        var bass = 0
        var mid = 0
        var treble = 0
        var beatValid = false
        var bpm = 0f
        var beatPhase = 0f
        var beatPeriodNanos = 0L

        var hasMotion = false
        var motionAtNanos = 0L
        var motion = 0
        var tiltX = 0
        var tiltY = 0

        internal fun copyFrom(other: Snapshot) {
            hasAudio = other.hasAudio
            audioAtNanos = other.audioAtNanos
            level = other.level
            bass = other.bass
            mid = other.mid
            treble = other.treble
            beatValid = other.beatValid
            bpm = other.bpm
            beatPhase = other.beatPhase
            beatPeriodNanos = other.beatPeriodNanos
            hasMotion = other.hasMotion
            motionAtNanos = other.motionAtNanos
            motion = other.motion
            tiltX = other.tiltX
            tiltY = other.tiltY
        }
    }
}
