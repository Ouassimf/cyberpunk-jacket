package com.ouassimf.jacket

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The protocol codec, the link contract with its fake jacket, the repository, the sound and motion analysers and
 * the live-mode logic are plain Kotlin: they must build and run without Android or the Bluetooth library. This
 * test is what keeps it that way.
 */
class LayeringTest {

    @Test
    fun `protocol, link, data, analysis and live packages import nothing from android or the bluetooth library`() {
        val offenders = listOf("protocol", "link", "data", "analysis", "live").flatMap { pkg ->
            imports(pkg).filter { (_, line) -> FORBIDDEN.any { line.startsWith("import $it") } }
        }
        assertTrue(offenders.joinToString("\n") { (file, line) -> "$file: $line" }, offenders.isEmpty())
    }

    @Test
    fun `the protocol package depends on no other package of the app`() {
        val offenders = imports("protocol").filter { (_, line) -> !line.startsWith("import com.ouassimf.jacket.protocol.") }
        assertTrue(offenders.joinToString("\n") { (file, line) -> "$file: $line" }, offenders.isEmpty())
    }

    @Test
    fun `the analysers are arithmetic and nothing else`() {
        // No coroutines, no threads, no I/O, nothing of the app: only the Kotlin maths functions.
        val offenders = imports("analysis").filter { (_, line) -> !line.startsWith("import kotlin.math.") }
        assertTrue(offenders.joinToString("\n") { (file, line) -> "$file: $line" }, offenders.isEmpty())
    }

    @Test
    fun `live mode's logic knows neither the screens nor the service that runs it`() {
        val offenders = listOf("live", "data", "link").flatMap { pkg ->
            imports(pkg).filter { (_, line) -> UPPER_LAYERS.any { line.startsWith("import com.ouassimf.jacket.$it.") } }
        }
        assertTrue(offenders.joinToString("\n") { (file, line) -> "$file: $line" }, offenders.isEmpty())
    }

    /** Every import line of every source file of [pkg], with the file it is in. */
    private fun imports(pkg: String): List<Pair<String, String>> {
        val root = File("src/main/kotlin/com/ouassimf/jacket")
        assertTrue("run from the app module directory, not ${File("").absolutePath}", root.isDirectory)
        val files = File(root, pkg).walkTopDown().filter { it.extension == "kt" }.toList()
        assertTrue("no sources found in $pkg", files.isNotEmpty())
        return files.flatMap { file -> file.readLines().map(String::trim).filter { it.startsWith("import ") }.map { file.name to it } }
    }

    private companion object {
        val FORBIDDEN = listOf("android.", "androidx.", "com.juul.")
        val UPPER_LAYERS = listOf("ui", "service", "ble")
    }
}
