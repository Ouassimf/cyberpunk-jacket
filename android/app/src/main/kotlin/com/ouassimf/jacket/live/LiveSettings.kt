package com.ouassimf.jacket.live

/** What the user chose on the Live screen. Kept across app restarts. */
data class LiveSettings(
    /** Listen with the microphone. */
    val sound: Boolean = true,
    /** Feel movement with the accelerometer. */
    val motion: Boolean = true,
    /** 0 to 100, 50 is neutral; for sound and motion alike. */
    val sensitivity: Int = 50,
    /**
     * Milliseconds by which the beat that is sent runs ahead of the beat that is heard, see [BeatClock]. Positive
     * makes the jacket flash earlier, to make up for the time a frame takes to reach the LEDs.
     */
    val latencyOffsetMs: Int = DEFAULT_OFFSET_MS,
) {
    /** With nothing to sense there is nothing to stream. */
    val anySource: Boolean get() = sound || motion

    /** The same settings with every number inside its range, for values read from storage. */
    fun sanitized() = copy(
        sensitivity = sensitivity.coerceIn(SENSITIVITY_RANGE),
        latencyOffsetMs = latencyOffsetMs.coerceIn(OFFSET_RANGE_MS),
    )

    companion object {
        val SENSITIVITY_RANGE = 0..100
        val OFFSET_RANGE_MS = -200..300

        /**
         * What is left to make up for once the phone has done its own sums. The phase is already measured for
         * the instant the sound reached the microphone (the system's capture timestamp) and carried forward to
         * the instant the frame is built, and the jacket takes out the jitter of the link with `t_ms`. Not
         * covered: the shortest a frame takes from here to the jacket's radio (about 5 to 10 ms at the short
         * connection interval), the jacket's wait for its next LED frame and the time to clock the LEDs out
         * (about 10 to 15 ms together), and the few milliseconds of microphone path a capture timestamp
         * leaves out. That is 20 to 35 ms. It is not rounded up: a flash a little after the beat passes for
         * on the beat, one before it does not.
         */
        const val DEFAULT_OFFSET_MS = 30
    }
}
