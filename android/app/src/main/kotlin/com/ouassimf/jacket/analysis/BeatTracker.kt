package com.ouassimf.jacket.analysis

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.sqrt

/**
 * Finds the tempo and the beat phase in a stream of onset strengths, one value per analysis frame
 * ([FRAME_RATE] per second). It never hears audio, so it can be tested with hand-made onset trains.
 *
 * How, and why this way:
 * - **Tempo** comes from the autocorrelation of the last [HISTORY] frames (6 s), recomputed four times a second.
 *   Every candidate beat period is scored by a comb: the mean autocorrelation at 1, 2, 3 and 4 periods. A period
 *   the music really repeats at scores high at all four, which also gives sub-frame resolution (an error of a
 *   quarter frame is a whole frame at the fourth tooth).
 * - **Half or double?** A beat and its half or double all repeat, so the comb alone cannot choose. Two rules do:
 *   a broad preference for tempos around 125 BPM, and "the fastest level that repeats as well wins": when the
 *   period half as long scores (nearly) the same, the pulses in between are as strong as the ones on the beat, so
 *   they are beats too. A plain click track at 174 BPM is therefore 174, not 87; a 90 BPM track whose off-beats
 *   are clearly weaker stays 90.
 * - **Lock** needs the same answer three times in a row with enough [confidence]; small changes are followed
 *   smoothly, a different tempo replaces the old one once it has been seen three times in a row.
 * - **Phase** is where a pulse train of that period best lines up with the recent onsets (recent beats count
 *   more). A free-running oscillator carries the phase from frame to frame and is nudged towards that
 *   measurement, so the phase is continuous and survives a bar without drums.
 * - **No beat** is an answer: [valid] is false when the comb is weak (noise, speech), when the tempo keeps
 *   changing, and when the last seconds were quiet compared to the rest or no longer match what came before
 *   (the music stopped).
 *
 * Cost: about 0.15 million multiply-adds per estimate, four estimates a second. Nothing is allocated after
 * construction.
 */
class BeatTracker {

    private val ring = FloatArray(HISTORY)
    private var head = 0
    private var filled = 0
    private var sinceEstimate = 0

    private val raw = FloatArray(HISTORY)
    private val prefix = FloatArray(HISTORY + 1)
    private val peaks = FloatArray(HISTORY)
    private val centred = FloatArray(HISTORY)
    private val acf = FloatArray(MAX_LAG + 2)
    private val scores = FloatArray(CANDIDATES)
    private val phaseScores = FloatArray((TAU_MAX / PHASE_STEP).toInt() + 2)
    private var maxLag = 0

    private var period = 0f
    private var oscillator = 0f
    private var pendingPeriod = 0f
    private var pendingCount = 0
    private var unsupported = 0
    private var phaseMisses = 0

    /** True while a beat is being tracked. When false, [bpm] and [phase] are not to be used. */
    var valid = false
        private set

    /** How periodic the onsets are at the chosen tempo: about 1 for a metronome, below 0.1 for noise. */
    var confidence = 0f
        private set

    /** The tracked beat period in frames, 0 when there has never been a lock or it was forgotten. */
    val periodFrames: Float get() = period

    val bpm: Float get() = if (period > 0f) FRAME_RATE * 60f / period else 0f

    /**
     * Where the latest frame lies in its beat, 0 until just under 1; 0 is the beat itself. This is the phase of
     * the onset stream: if onsets reach [add] late, the caller adds that delay.
     */
    val phase: Float get() = oscillator

    fun add(onset: Float) {
        ring[head] = onset
        if (++head == HISTORY) head = 0
        if (filled < HISTORY) filled++
        if (period > 0f) {
            oscillator += 1f / period
            if (oscillator >= 1f) oscillator -= 1f
        }
        if (++sinceEstimate >= ESTIMATE_EVERY) {
            sinceEstimate = 0
            if (filled >= MIN_HISTORY) estimate()
        }
    }

    fun reset() {
        ring.fill(0f)
        head = 0
        filled = 0
        sinceEstimate = 0
        period = 0f
        oscillator = 0f
        pendingCount = 0
        unsupported = 0
        phaseMisses = 0
        valid = false
        confidence = 0f
    }

    private fun estimate() {
        val n = filled
        var index = head - n
        if (index < 0) index += HISTORY
        for (i in 0 until n) {
            raw[i] = ring[index]
            if (++index == HISTORY) index = 0
        }
        // Keep what stands out of its surroundings: each value less the mean of the 0.8 s around it, negatives
        // dropped. A level that slowly rises or falls would otherwise correlate with itself at every lag and
        // pass for a beat of any tempo.
        prefix[0] = 0f
        for (i in 0 until n) prefix[i + 1] = prefix[i] + raw[i]
        var peakSum = 0f
        for (i in 0 until n) {
            val from = maxOf(0, i - LOCAL_MEAN)
            val to = minOf(n, i + LOCAL_MEAN + 1)
            val peak = maxOf(0f, raw[i] - (prefix[to] - prefix[from]) / (to - from))
            peaks[i] = peak
            peakSum += peak
        }
        val peakMean = peakSum / n
        // Smoothed over five frames (1 2 3 2 1) for the autocorrelation: its peaks become wide enough to read
        // between whole lags, and a drummer who is a frame early or late still counts.
        var power = 0f
        for (i in 0 until n) {
            val c = (peaks[maxOf(0, i - 2)] + peaks[minOf(n - 1, i + 2)] +
                2f * (peaks[maxOf(0, i - 1)] + peaks[minOf(n - 1, i + 1)]) +
                3f * peaks[i]) * (1f / 9f) - peakMean
            centred[i] = c
            power += c * c
        }
        power /= n
        var recent = 0f
        for (i in n - RECENT until n) recent += peaks[i]
        recent /= RECENT

        // Silence, or the music stopped: the six seconds still hold a beat, but nobody is playing it any more.
        // Judged by what stands out, like everything else here: a background that merely got quieter is not it.
        if (power < SILENCE || recent < RECENT_SHARE * peakMean) {
            confidence = 0f
            pendingCount = 0
            noSupport()
            valid = false
            return
        }

        maxLag = minOf(MAX_LAG, n - MIN_OVERLAP)
        for (lag in MIN_LAG..maxLag + 1) {
            var s = 0f
            for (i in lag until n) s += centred[i] * centred[i - lag]
            acf[lag] = s / ((n - lag) * power) // unbiased, so a long lag is not at a disadvantage
        }

        var best = 0
        var bestWeighted = Float.NEGATIVE_INFINITY
        var scoreSum = 0f
        for (i in 0 until CANDIDATES) {
            val tau = TAU_MIN + i * TAU_STEP
            val score = comb(tau)
            scores[i] = score
            scoreSum += score
            var weighted = score * PRIOR[i]
            if (period > 0f && near(tau, period)) weighted *= STICKINESS
            if (weighted > bestWeighted) {
                bestWeighted = weighted
                best = i
            }
        }

        // The fastest level that repeats as well wins. With hysteresis, so a lock does not flip between the two.
        var chosen = best
        val bestTau = TAU_MIN + best * TAU_STEP
        val halfTau = bestTau / 2
        if (halfTau >= TAU_MIN) {
            val half = peakNear(((halfTau - TAU_MIN) / TAU_STEP + 0.5f).toInt())
            val h = TAU_MIN + half * TAU_STEP
            val needed = when {
                period > 0f && near(h, period) -> HALVE_TO_KEEP
                period > 0f && near(bestTau, period) -> HALVE_TO_SWITCH
                else -> HALVE_RATIO
            }
            // The pulses in between show at odd multiples of the half period, the beats themselves at even ones.
            val far = 4 * h <= maxLag
            val between = if (far) at(h) + at(3 * h) else at(h)
            val onBeat = if (far) at(2 * h) + at(4 * h) else at(2 * h)
            if (between >= needed * onBeat) chosen = half
        }

        var tau = TAU_MIN + chosen * TAU_STEP
        val isPeak = chosen in 1 until CANDIDATES - 1 && scores[chosen] >= scores[chosen - 1] && scores[chosen] >= scores[chosen + 1]
        if (isPeak) tau += vertex(scores[chosen - 1], scores[chosen], scores[chosen + 1]) * TAU_STEP
        // Both must hold: the beat repeats across the whole history, and the last seconds still belong to it.
        // And it must do so better than a period picked blindly would; the end of the range is not a tempo.
        confidence = if (isPeak) minOf(support(tau), recentSupport(tau, n)) - maxOf(0f, scoreSum / CANDIDATES) else 0f

        var relocked = false
        if (confidence >= MIN_CONFIDENCE && period > 0f && near(tau, period)) {
            period += PERIOD_GAIN * (tau - period)
            unsupported = 0
            pendingCount = 0
        } else if (confidence >= MIN_CONFIDENCE) {
            pendingCount = if (pendingCount > 0 && near(tau, pendingPeriod)) pendingCount + 1 else 1
            pendingPeriod = tau
            if (pendingCount >= CONFIRMATIONS) {
                period = tau
                pendingCount = 0
                unsupported = 0
                relocked = true
            } else {
                noSupport()
            }
        } else {
            pendingCount = 0
            noSupport()
        }

        if (period > 0f && unsupported == 0) {
            trackPhase(n, relocked)
            valid = true
        }
    }

    /** One more estimate that did not back the current lock: after a while it stops counting, later it is forgotten. */
    private fun noSupport() {
        if (period == 0f) {
            valid = false
            return
        }
        unsupported++
        if (unsupported >= DROP_AFTER) valid = false
        if (unsupported >= FORGET_AFTER) {
            period = 0f
            oscillator = 0f
            unsupported = 0
        }
    }

    /** The autocorrelation at a lag between two whole ones. */
    private fun at(lag: Float): Float {
        val i = lag.toInt()
        return acf[i] + (acf[i + 1] - acf[i]) * (lag - i)
    }

    /** Mean autocorrelation at the first [COMB_TEETH] multiples of [tau] that the history is long enough for. */
    private fun comb(tau: Float): Float {
        var total = 0f
        var used = 0
        var lag = tau
        while (used < COMB_TEETH && lag <= maxLag) {
            total += at(lag)
            used++
            lag += tau
        }
        return if (used == 0) 0f else total / used
    }

    /**
     * The comb again, without its best tooth: two sounds that happen to be one period apart make one tall tooth,
     * a beat makes all of them. Zero until the history is long enough for three teeth.
     */
    private fun support(tau: Float): Float {
        var total = 0f
        var highest = Float.NEGATIVE_INFINITY
        var used = 0
        var lag = tau
        while (used < COMB_TEETH && lag <= maxLag) {
            val value = at(lag)
            total += value
            if (value > highest) highest = value
            used++
            lag += tau
        }
        return if (used < MIN_TEETH) 0f else (total - highest) / (used - 1)
    }

    /**
     * How well the last [RECENT_SPAN] frames match the frames one, two and three periods before them, as a
     * correlation coefficient. When the music stops and room noise takes over, the whole history still looks
     * periodic for seconds; this does not.
     */
    private fun recentSupport(tau: Float, n: Int): Float {
        val start = n - RECENT_SPAN
        var total = 0f
        var used = 0
        var lag = tau
        while (used < RECENT_TEETH && start - lag >= 1f) {
            val whole = lag.toInt()
            val part = lag - whole
            var ab = 0f
            var aa = 0f
            var bb = 0f
            for (i in start until n) {
                val a = centred[i]
                val b = centred[i - whole] + (centred[i - whole - 1] - centred[i - whole]) * part
                ab += a * b
                aa += a * a
                bb += b * b
            }
            if (aa > 0f && bb > 0f) total += ab / sqrt(aa * bb)
            used++
            lag += tau
        }
        return if (used == 0) 0f else total / used
    }

    private fun peakNear(index: Int): Int {
        var best = index.coerceIn(0, CANDIDATES - 1)
        for (i in maxOf(0, index - HALF_SEARCH)..minOf(CANDIDATES - 1, index + HALF_SEARCH)) if (scores[i] > scores[best]) best = i
        return best
    }

    /** Lines a pulse train of the locked period up with the onsets and pulls the oscillator towards it. */
    private fun trackPhase(n: Int, jump: Boolean) {
        val tau = period
        val last = (n - 1).toFloat()
        val fade = exp(-tau / PHASE_MEMORY)
        val count = minOf(phaseScores.size, (tau / PHASE_STEP).toInt() + 1)
        var best = 0
        for (j in 0 until count) {
            var position = last - j * PHASE_STEP
            var weight = 1f
            var s = 0f
            while (position >= 0f) {
                val i = position.toInt()
                val next = if (i + 1 < n) peaks[i + 1] else peaks[i]
                s += weight * (peaks[i] + (next - peaks[i]) * (position - i))
                weight *= fade
                position -= tau
            }
            phaseScores[j] = s
            if (s > phaseScores[best]) best = j
        }
        val before = phaseScores[if (best == 0) count - 1 else best - 1]
        val after = phaseScores[if (best == count - 1) 0 else best + 1]
        val sinceBeat = (best + vertex(before, phaseScores[best], after)) * PHASE_STEP
        var measured = sinceBeat / tau
        measured -= floor(measured)

        var error = measured - oscillator
        error -= floor(error + 0.5f) // the short way round
        when {
            jump -> {
                oscillator = measured
                phaseMisses = 0
            }
            abs(error) <= PHASE_PULL_RANGE -> {
                oscillator += PHASE_GAIN * error
                oscillator -= floor(oscillator)
                phaseMisses = 0
            }
            // Far off: one such measurement is an accident (a fill, a clap), several in a row are the truth.
            ++phaseMisses >= CONFIRMATIONS -> {
                oscillator = measured
                phaseMisses = 0
            }
        }
    }

    companion object {
        const val FRAME_RATE = 100

        /** Tempos reported. The search goes a little further out so that both ends are proper peaks. */
        const val MIN_BPM = 70f
        const val MAX_BPM = 180f

        /** Six seconds of onsets: enough for four beats at the slowest tempo, short enough to follow a change. */
        const val HISTORY = 600
        const val MIN_HISTORY = 350
        const val ESTIMATE_EVERY = 25

        private const val TAU_MIN = FRAME_RATE * 60f / 186f
        private const val TAU_MAX = FRAME_RATE * 60f / 66f
        private const val TAU_STEP = 0.2f
        private val CANDIDATES = ((TAU_MAX - TAU_MIN) / TAU_STEP).toInt() + 1
        private const val COMB_TEETH = 4
        private val MAX_LAG = (COMB_TEETH * TAU_MAX).toInt() + 1
        private const val MIN_LAG = 15
        private const val MIN_OVERLAP = 150
        private const val MIN_TEETH = 3

        /** A lock needs this much of the onset energy to repeat at the beat. Noise stays under 0.1. */
        const val MIN_CONFIDENCE = 0.3f
        private const val CONFIRMATIONS = 3
        private const val DROP_AFTER = 6
        private const val FORGET_AFTER = 40
        private const val PERIOD_GAIN = 0.5f
        private const val STICKINESS = 1.1f
        private const val NEAR = 0.04f

        private const val HALVE_RATIO = 0.93f
        private const val HALVE_TO_KEEP = 0.85f
        private const val HALVE_TO_SWITCH = 1.0f
        private const val HALF_SEARCH = 3

        private const val PHASE_STEP = 0.5f
        private const val PHASE_MEMORY = 250f
        private const val PHASE_GAIN = 0.5f
        private const val PHASE_PULL_RANGE = 0.15f

        private const val LOCAL_MEAN = 40
        private const val RECENT = 200
        private const val RECENT_SPAN = 250
        private const val RECENT_TEETH = 3
        private const val RECENT_SHARE = 0.2f
        private const val SILENCE = 1e-9f

        /** A gentle preference for tempos around 125 BPM, 1.2 octaves wide: it only decides near-ties. */
        private val PRIOR = FloatArray(CANDIDATES) {
            val octaves = ln((FRAME_RATE * 60f / (TAU_MIN + it * TAU_STEP)) / 125f) / ln(2f)
            exp(-0.5f * (octaves / 1.2f) * (octaves / 1.2f))
        }

        private fun near(a: Float, b: Float) = abs(a - b) <= NEAR * b

        /** Offset, in steps, of the vertex of the parabola through three equally spaced values around a maximum. */
        private fun vertex(before: Float, at: Float, after: Float): Float {
            val curve = before - 2 * at + after
            return if (curve >= 0f) 0f else (0.5f * (before - after) / curve).coerceIn(-0.5f, 0.5f)
        }
    }
}
