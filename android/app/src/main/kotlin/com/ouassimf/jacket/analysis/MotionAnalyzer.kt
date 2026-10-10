package com.ouassimf.jacket.analysis

import kotlin.math.exp
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * Turns accelerometer samples into how much the wearer moves and how the phone is tilted. Plain Kotlin; the
 * Android side only feeds it sensor events.
 *
 * - **Gravity** is the acceleration low-passed over 0.4 s. The accelerometer alone is enough for that and is on
 *   every phone, which the fused gravity and linear-acceleration sensors are not; they would also keep the gyro
 *   powered.
 * - **Motion** is the size of what is left once gravity is taken away, less a small dead zone that hides sensor
 *   noise and a hand that is merely not perfectly still. It rises within a sample or two and falls over 100 ms:
 *   just enough to turn the back and forth of a movement into its strength, the jacket does the smoothing
 *   (about 60 ms up, 400 ms down). It is then scaled against a ceiling that follows the strongest recent movement (quick up, two seconds of hold,
 *   then down by a quarter each second) but never goes below what a walk produces. So a walk already registers
 *   clearly, and dancing or shaking still has somewhere to go after a few seconds of it.
 * - **Tilt** is the direction of gravity in the phone's own axes: x towards the right edge of the screen, y
 *   towards its top. Each is 127 times that component of the unit gravity vector, i.e. 127 times the sine of
 *   the angle by which that axis points up. Flat on a table both are 0; held upright [tiltY] is +127; on its
 *   left long edge with the right edge up [tiltX] is +127.
 *
 * Samples may come at any rate: every filter uses the time between samples. Nothing is allocated. Not
 * thread-safe: one thread feeds it and reads it.
 */
class MotionAnalyzer {

    /** 0 to 100, 50 is neutral. Higher makes small movements count for more (down to half the neutral ceiling). */
    var sensitivity: Int = 50

    /** Movement energy, 0 (still) to 255 (as vigorous as it has recently been, or more). */
    var motion = 0
        private set

    /** -127 to 127, see the class description. */
    var tiltX = 0
        private set
    var tiltY = 0
        private set

    private var started = false
    private var lastNanos = 0L
    private var gx = 0f
    private var gy = 0f
    private var gz = 0f
    private var envelope = 0f
    private var ceiling = 0f
    private var held = 0f

    /** One accelerometer sample in m/s², axes as Android reports them, with its timestamp in nanoseconds. */
    fun add(x: Float, y: Float, z: Float, timestampNanos: Long) {
        if (!started) {
            started = true
            lastNanos = timestampNanos
            gx = x; gy = y; gz = z
            updateTilt()
            return
        }
        val dt = ((timestampNanos - lastNanos) * 1e-9f).coerceIn(MIN_STEP, MAX_STEP)
        lastNanos = timestampNanos

        val follow = 1f - exp(-dt / GRAVITY_TIME)
        gx += (x - gx) * follow
        gy += (y - gy) * follow
        gz += (z - gz) * follow
        updateTilt()

        val dx = x - gx
        val dy = y - gy
        val dz = z - gz
        val moving = maxOf(0f, sqrt(dx * dx + dy * dy + dz * dz) - DEAD_ZONE)
        envelope += (moving - envelope) * (1f - exp(-dt / if (moving > envelope) ATTACK_TIME else RELEASE_TIME))

        val lowest = WALKING * exp((50 - sensitivity.coerceIn(0, 100)) * SENSITIVITY_STEP)
        if (envelope > ceiling) {
            ceiling += (envelope - ceiling) * (1f - exp(-dt / CEILING_ATTACK_TIME))
            held = CEILING_HOLD
        } else if (held > 0f) {
            held -= dt
        } else {
            ceiling *= exp(-dt * CEILING_DECAY)
        }
        if (ceiling < lowest) ceiling = lowest
        motion = (255f * envelope / ceiling).roundToInt().coerceIn(0, 255)
    }

    fun reset() {
        started = false
        envelope = 0f
        ceiling = 0f
        held = 0f
        motion = 0
        tiltX = 0
        tiltY = 0
    }

    private fun updateTilt() {
        val size = sqrt(gx * gx + gy * gy + gz * gz)
        if (size < FREE_FALL) return // no direction to speak of: keep the last one
        tiltX = (127f * gx / size).roundToInt().coerceIn(-127, 127)
        tiltY = (127f * gy / size).roundToInt().coerceIn(-127, 127)
    }

    private companion object {
        const val MIN_STEP = 0.001f
        const val MAX_STEP = 0.2f
        const val GRAVITY_TIME = 0.4f
        const val DEAD_ZONE = 0.3f // m/s²
        const val ATTACK_TIME = 0.02f
        const val RELEASE_TIME = 0.1f

        /** The ceiling's lowest value at neutral sensitivity, m/s²: about the peaks of a brisk walk. */
        const val WALKING = 4f
        const val SENSITIVITY_STEP = 0.01386f // ln(2) / 50: half at 100, double at 0
        const val CEILING_ATTACK_TIME = 0.3f
        const val CEILING_HOLD = 2f
        const val CEILING_DECAY = 0.288f // per second: down by a quarter
        const val FREE_FALL = 1f
    }
}
