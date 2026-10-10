package com.ouassimf.jacket.link

import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

/**
 * How the Bluetooth link looks for a jacket it lost: the numbers, kept apart from the Bluetooth code so they can
 * be checked against what the board was measured to do.
 *
 * The jacket, as observed: it does not advertise while connected, and is on the air again 0.3 to 3.2 s after a
 * disconnect. It then advertises every 150 ms or so for 30 s, and every 550 ms after that. Its advertisement
 * carries only the flags and the 128-bit service UUID; the name sits in the scan response, which a phone at
 * the edge of range may never get. So the jacket is found by its service and by nothing else.
 */
object ReconnectPolicy {

    /**
     * After a drop the same device is tried again directly this many times, which needs no scan and is the
     * fastest way back; then it is forgotten and looked for from scratch.
     */
    const val RESCAN_AFTER_FAILURES = 3

    /** How long a scan stays in the eager, power-hungry mode before it settles for the low-power one. */
    val FAST_SCAN = 15.seconds

    /** The pause before attempt number [failures] + 1: 0.5 s, 1 s, 2 s, 4 s, 8 s, then 15 s. */
    fun backoff(failures: Int): Duration =
        (500.milliseconds * (1 shl (failures - 1).coerceIn(0, 5))).coerceAtMost(15.seconds)
}
