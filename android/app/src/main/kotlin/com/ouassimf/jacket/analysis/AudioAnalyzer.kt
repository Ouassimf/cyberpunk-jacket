package com.ouassimf.jacket.analysis

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln

/**
 * Turns microphone samples into what the jacket's effects react to: how loud it is overall and in three bands,
 * where the onsets are, and the tempo and phase of the beat. Plain Kotlin and plain arithmetic, so it runs and is
 * tested on any JVM; the Android side only feeds it.
 *
 * Input is mono 16-bit PCM at [SAMPLE_RATE] in blocks of any size. Every [HOP] samples (10 ms) one analysis
 * frame is made, and the outputs below change.
 *
 * The choices, briefly:
 * - **16 kHz** is a rate every Android microphone delivers and reaches 8 kHz, which covers kick to hi-hat. A
 *   higher rate would cost more for nothing an LED can show.
 * - **One 512-point FFT per frame** (32 ms Hann window, 69 % overlap) serves everything: its bins are summed
 *   into 17 roughly logarithmic bands, and those into bass (60 to 265 Hz), mid (to 2 kHz) and treble (to 8 kHz).
 *   A filter bank would be about as cheap for the three levels but worse for onsets.
 * - **Automatic gain** works in decibels. A ceiling per output follows the loudest recent sound. It rises
 *   within a few frames; while sound keeps coming within 3 dB of it, it stays (creeping down 2 dB a second, to
 *   settle exactly on a steady level); 1.5 s after the last such sound it sinks 8 dB a second until sound meets
 *   it again. So both a quiet room and a club end up using the whole range within a few seconds, and between
 *   the hits of a steady beat the ceiling does not move, which keeps a hit half as loud at half the height.
 *   The outputs are linear in sound pressure below that ceiling (255 at the ceiling, 128 about 6 dB under, 0
 *   from 30 dB under). The ceiling never sinks below an absolute floor, so the hiss of a silent room is not
 *   blown up into a light show; each band's ceiling stays above that floor too and within 24 dB of the overall
 *   one, so a band holding only leakage or noise stays dark.
 * - **Smoothing** is left to the jacket, which does its own (about 25 ms up, 180 ms down). The outputs here
 *   follow the sound: up at once, down with a 30 ms time constant, which only irons out the frame-to-frame
 *   wobble of a 32 ms window. Whoever samples them more slowly than every frame should take the peak since
 *   the last sample, as `LiveFeatures` does, so that a short hit is never missed.
 * - **Onsets** are the summed rise of the log energy of the 17 bands from one frame to the next (spectral flux),
 *   with the bass bands counted double because the kick carries the beat. The log makes a soft hi-hat count
 *   next to a loud bass line; a floor 42 dB under the ceiling keeps noise out of it.
 * - **Tempo and beat phase** come from that onset stream, see [BeatTracker].
 *
 * Nothing is allocated after construction. Not thread-safe: one thread feeds it and reads it.
 */
class AudioAnalyzer {

    /**
     * 0 to 100, 50 is neutral. Higher lets quieter sound through (the absolute floor drops by up to 12 dB) and
     * drives the outputs harder (up to 6 dB); lower does the opposite.
     */
    var sensitivity: Int = 50

    /** Overall loudness, 0 to 255, after gain and smoothing. */
    var level = 0
        private set
    var bass = 0
        private set
    var mid = 0
        private set
    var treble = 0
        private set

    /** True when the frame analysed last started a new sound (a drum hit, a clap). */
    var onset = false
        private set

    /** Onsets since construction or [reset]. */
    var onsets = 0L
        private set

    /** Frames analysed since construction or [reset]. */
    var frames = 0L
        private set

    /** True while a beat is tracked; false in silence and for sound without a steady pulse. */
    val beatValid: Boolean get() = tracker.valid

    /** Beats per minute, 0 while [beatValid] is false. */
    val bpm: Float get() = if (tracker.valid) tracker.bpm else 0f

    /** Seconds per beat, 0 while [beatValid] is false. */
    val beatPeriod: Float get() = if (tracker.valid) tracker.periodFrames / FRAME_RATE else 0f

    val beatConfidence: Float get() = tracker.confidence

    /**
     * Where the last sample given to [process] lies in its beat: 0 on the beat, rising to just under 1 before the
     * next. Only meaningful while [beatValid].
     */
    val beatPhase: Float
        get() {
            val period = tracker.periodFrames
            if (period <= 0f) return 0f
            // The tracker's phase belongs to the onset stream, which sees a hit ONSET_DELAY frames after it sounded.
            val phase = tracker.phase + (ONSET_DELAY + pending.toFloat() / HOP) / period
            return phase - floor(phase)
        }

    private val tracker = BeatTracker()
    private val fft = Fft(WINDOW)
    private val window = FloatArray(WINDOW) { (0.5 - 0.5 * cos(2.0 * PI * it / WINDOW)).toFloat() }
    private val samples = FloatArray(WINDOW)
    private val re = FloatArray(WINDOW)
    private val im = FloatArray(WINDOW)
    private var write = 0
    private var pending = 0

    private val bandLog = FloatArray(BANDS)
    private var primed = false
    private var fluxReference = Float.NaN
    private var fluxMean = 0f
    private var fluxDeviation = 0f
    private var sinceOnset = REFRACTORY

    // Index 0 is the overall level, 1 to 3 are bass, mid and treble.
    private val decibels = FloatArray(4)
    private val ceiling = FloatArray(4) { Float.NEGATIVE_INFINITY }
    private val hold = IntArray(4)
    private val smoothed = FloatArray(4)

    fun process(pcm: ShortArray, offset: Int = 0, length: Int = pcm.size - offset) {
        for (i in offset until offset + length) {
            samples[write] = pcm[i] * (1f / 32768f)
            write = (write + 1) and (WINDOW - 1)
            if (++pending == HOP) {
                pending = 0
                analyse()
            }
        }
    }

    fun reset() {
        samples.fill(0f)
        write = 0
        pending = 0
        primed = false
        fluxReference = Float.NaN
        fluxMean = 0f
        fluxDeviation = 0f
        sinceOnset = REFRACTORY
        ceiling.fill(Float.NEGATIVE_INFINITY)
        hold.fill(0)
        smoothed.fill(0f)
        level = 0; bass = 0; mid = 0; treble = 0
        onset = false
        onsets = 0
        frames = 0
        tracker.reset()
    }

    private fun analyse() {
        var read = write // the oldest sample
        for (i in 0 until WINDOW) {
            re[i] = samples[read] * window[i]
            im[i] = 0f
            read = (read + 1) and (WINDOW - 1)
        }
        fft.transform(re, im)

        val gain = (sensitivity.coerceIn(0, 100) - 50) * GAIN_PER_STEP
        val floorDb = CEILING_FLOOR - gain
        // The onset floor follows the level ceiling slowly, so the ceiling's own movements are not onsets.
        val reference = maxOf(ceiling[0], floorDb)
        fluxReference = if (fluxReference.isNaN()) reference else fluxReference + (reference - fluxReference) * FLUX_REFERENCE_RATE
        val floorPower = exp((fluxReference - FLUX_RANGE) * DB_TO_LN_POWER)

        var bassPower = 0f
        var midPower = 0f
        var treblePower = 0f
        var flux = 0f
        for (band in 0 until BANDS) {
            var energy = 0f
            for (k in BAND_START[band] until BAND_START[band + 1]) energy += re[k] * re[k] + im[k] * im[k]
            energy *= POWER_SCALE
            when {
                band < BASS_BANDS -> bassPower += energy
                band < BASS_BANDS + MID_BANDS -> midPower += energy
                else -> treblePower += energy
            }
            val log = ln(energy + floorPower)
            if (primed) {
                val rise = log - bandLog[band]
                if (rise > 0f) flux += if (band < BASS_BANDS) BASS_WEIGHT * rise else rise
            }
            bandLog[band] = log
        }
        primed = true

        decibels[0] = toDecibels(bassPower + midPower + treblePower)
        decibels[1] = toDecibels(bassPower)
        decibels[2] = toDecibels(midPower)
        decibels[3] = toDecibels(treblePower)
        for (i in 0 until 4) {
            val value = decibels[i]
            var top = ceiling[i]
            if (value > top) {
                top += (value - top) * CEILING_ATTACK
                hold[i] = CEILING_HOLD
            } else if (value > top - CEILING_NEAR) {
                // As loud as it gets, again: this is still the level to measure against. It only creeps down,
                // so that it ends up exactly on a sound that stays just under it.
                hold[i] = CEILING_HOLD
                top -= CEILING_CREEP
            } else if (hold[i] > 0) {
                hold[i]--
            } else {
                top -= CEILING_DECAY
            }
            val lowest = if (i == 0) floorDb else maxOf(ceiling[0] - BAND_SPREAD, floorDb)
            if (top < lowest || top.isNaN()) top = lowest
            ceiling[i] = top

            val amplitude = exp((value - top + gain * PUSH_SHARE) * DB_TO_LN_AMPLITUDE)
            val target = ((amplitude - GATE) / (1f - GATE)).coerceIn(0f, 1f)
            smoothed[i] += (target - smoothed[i]) * if (target > smoothed[i]) OUTPUT_ATTACK else OUTPUT_RELEASE
        }
        level = toByte(smoothed[0])
        bass = toByte(smoothed[1])
        mid = toByte(smoothed[2])
        treble = toByte(smoothed[3])

        // An onset is a frame whose flux stands out from the last second or so, at most one per 60 ms. Not in
        // the first half second: until then there is no "usual" to stand out from.
        sinceOnset++
        onset = frames >= ONSET_WARM_UP && sinceOnset >= REFRACTORY &&
            flux > fluxMean + ONSET_DEVIATIONS * fluxDeviation + ONSET_MINIMUM
        if (onset) {
            sinceOnset = 0
            onsets++
        }
        val rate = maxOf(FLUX_STATS_RATE, 1f / (frames + 1)) // a plain average at first, so it settles at once
        fluxDeviation += (abs(flux - fluxMean) - fluxDeviation) * rate
        fluxMean += (flux - fluxMean) * rate

        // Anything smaller is arithmetic, not sound: the ripple a pure tone leaves in the bands next to it
        // repeats exactly, and the tracker, which only looks at the shape, would call that a beat.
        tracker.add(if (flux >= FLUX_MINIMUM) flux else 0f)
        frames++
    }

    companion object {
        const val SAMPLE_RATE = 16_000

        /** Samples per analysis frame: 10 ms. */
        const val HOP = 160
        const val FRAME_RATE = BeatTracker.FRAME_RATE
        private const val WINDOW = 512

        /** First FFT bin of each band; a bin is 31.25 Hz wide. Bin 0 and 1 are left out: DC and rumble. */
        private val BAND_START = intArrayOf(2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 41, 52, 65, 85, 110, 145, 195, 256)
        private val BANDS = BAND_START.size - 1
        private const val BASS_BANDS = 5 // bins 2 to 8: 47 to 265 Hz
        private const val MID_BANDS = 7 // bins 9 to 64: up to 2 kHz; the rest, up to 8 kHz, is treble

        /** Makes the summed bins the mean square of the signal (full-scale sine: 0.5, which is -3 dB). */
        private const val POWER_SCALE = 16f / (3f * WINDOW * WINDOW)

        private const val DB_TO_LN_POWER = 0.2302585f // ln(10) / 10
        private const val DB_TO_LN_AMPLITUDE = 0.1151293f // ln(10) / 20
        private const val LN_TO_DB = 4.3429448f // 10 / ln(10)

        /** The lowest the level ceiling goes at neutral sensitivity, in dB below full scale: a quiet conversation. */
        private const val CEILING_FLOOR = -50f
        private const val GAIN_PER_STEP = 0.24f // 12 dB over 50 steps of sensitivity
        private const val PUSH_SHARE = 0.5f
        private const val CEILING_ATTACK = 0.3f // per frame: a hit of a few frames takes it most of the way
        private const val CEILING_HOLD = 150 // frames
        private const val CEILING_NEAR = 3f // dB
        private const val CEILING_CREEP = 0.02f // dB per frame: 2 dB a second
        private const val CEILING_DECAY = 0.08f // dB per frame: 8 dB a second
        private const val BAND_SPREAD = 24f

        /** Sound this far under the ceiling, as an amplitude ratio (30 dB), reads 0. */
        private const val GATE = 0.0316f
        private const val OUTPUT_ATTACK = 1f // per frame: at once
        private const val OUTPUT_RELEASE = 0.283f // per frame: 30 ms

        private const val FLUX_RANGE = 42f
        private const val FLUX_REFERENCE_RATE = 0.01f
        private const val BASS_WEIGHT = 2f
        private const val FLUX_STATS_RATE = 0.01f
        private const val ONSET_DEVIATIONS = 3.5f
        private const val ONSET_MINIMUM = 1.5f
        private const val ONSET_WARM_UP = 50
        private const val FLUX_MINIMUM = 1f
        private const val REFRACTORY = 6

        /**
         * Frames between a hit and the frame whose flux shows it most: the hit enters at the tapered end of the
         * window and counts fully about a frame later. Measured with clicks and kicks, see the tests.
         */
        const val ONSET_DELAY = 1.3f

        private fun toDecibels(power: Float) = ln(power + 1e-12f) * LN_TO_DB

        private fun toByte(value: Float) = (value * 255f + 0.5f).toInt().coerceIn(0, 255)
    }
}
