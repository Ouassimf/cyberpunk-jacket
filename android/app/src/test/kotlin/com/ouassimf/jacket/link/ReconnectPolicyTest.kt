package com.ouassimf.jacket.link

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

/** The reconnect numbers against what the real board was measured to do. */
class ReconnectPolicyTest {

    @Test
    fun `the pauses grow from half a second to fifteen and stay there`() {
        assertEquals(
            listOf(500, 1_000, 2_000, 4_000, 8_000, 15_000, 15_000, 15_000),
            (1..8).map { ReconnectPolicy.backoff(it).inWholeMilliseconds.toInt() },
        )
        assertEquals("nonsense in, the shortest pause out", 500.milliseconds, ReconnectPolicy.backoff(0))
        assertEquals(500.milliseconds, ReconnectPolicy.backoff(-3))
        assertEquals(15.seconds, ReconnectPolicy.backoff(Int.MAX_VALUE))
    }

    @Test
    fun `the direct attempts cover the time the jacket needs to advertise again`() {
        // It is back on the air 0.3 to 3.2 s after a disconnect. The attempts before the link gives up on the
        // known device and scans must not all be over before that.
        var elapsed = Duration.ZERO
        val starts = (1..ReconnectPolicy.RESCAN_AFTER_FAILURES).map { failures ->
            elapsed += ReconnectPolicy.backoff(failures)
            elapsed
        }
        assertTrue("the first attempt comes early: ${starts.first()}", starts.first() <= 1.seconds)
        assertTrue("the last direct attempt starts after 3.2 s: ${starts.last()}", starts.last() >= 3.2.seconds)
    }

    @Test
    fun `the eager scan hears many advertisements, also at the slow rate`() {
        // Every 150 ms for the first 30 s, every 550 ms after. A handful may be lost at weak signal.
        assertTrue(ReconnectPolicy.FAST_SCAN >= 550.milliseconds * 20)
        assertTrue("and does not outlast the fast advertising for nothing", ReconnectPolicy.FAST_SCAN <= 30.seconds)
    }

    @Test
    fun `the jacket is looked for by its service, never by its name`() {
        // The name is only in the scan response, which is sometimes missed. This cannot be run on a JVM, so it
        // is read: the Bluetooth code filters on the service UUID and never mentions a name.
        val source = File("src/main/kotlin/com/ouassimf/jacket/ble/BleJacketLink.kt").readText()
        assertTrue(source.contains("match { services = listOf(SERVICE) }"))
        val code = source.lines().map { it.substringBefore("//").trim() }.filterNot { it.startsWith("*") || it.startsWith("/*") }
        assertFalse(code.any { Regex("""\bname\b|DEVICE_NAME|"Jacket"""").containsMatchIn(it) })
        // Nor does it pair: the jacket uses no pairing or encryption.
        assertFalse(code.any { it.contains("createBond") || it.contains("bond", ignoreCase = true) })
    }
}
