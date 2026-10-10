package com.ouassimf.jacket.ui

import android.Manifest
import android.app.Activity
import android.app.Notification
import android.content.Context
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.hardware.Sensor
import android.hardware.SensorManager
import android.os.Looper
import android.os.SystemClock
import androidx.activity.compose.setContent
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasScrollAction
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasScrollToNodeAction
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isDialog
import androidx.compose.ui.test.isSelectable
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.test.performTextInput
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipe
import androidx.compose.ui.test.swipeLeft
import androidx.compose.ui.test.swipeRight
import androidx.compose.ui.test.click
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import com.ouassimf.jacket.JacketApplication
import com.ouassimf.jacket.MainActivity
import com.ouassimf.jacket.data.Change
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.link.FakeJacket
import com.ouassimf.jacket.link.LinkStatus
import com.ouassimf.jacket.link.RecordingLink
import com.ouassimf.jacket.link.RecordingLink.Write
import com.ouassimf.jacket.link.ScriptedLink
import com.ouassimf.jacket.live.LiveController
import com.ouassimf.jacket.live.LiveFeatures
import com.ouassimf.jacket.live.LiveNotice
import com.ouassimf.jacket.live.LiveSettings
import com.ouassimf.jacket.service.LiveService
import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.EffectInfo
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.PaletteInfo
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.ParamFlags
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry
import com.ouassimf.jacket.protocol.with
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.SensorEventBuilder
import org.robolectric.shadows.ShadowPowerManager
import org.robolectric.shadows.ShadowSensor
import org.robolectric.util.ReflectionHelpers
import java.io.File
import java.time.Duration
import kotlin.math.abs
import kotlin.math.sin

/**
 * The real activity and screens, rendered on the JVM by Robolectric. Checks what the screens promise (the effect
 * grid mirrors the jacket's catalogue, only the declared controls show, gestures reach the wire) without a phone.
 *
 * Run with `-Pjacket.screenshots=<dir>` to also get a PNG of each screen for a look at the layout.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "w411dp-h891dp-xxhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ScreensTest {

    @get:Rule
    val compose = createAndroidComposeRule<MainActivity>()

    private val scope = CoroutineScope(Dispatchers.Main.immediate + Job())
    private val app get() = compose.activity.application as JacketApplication

    @After
    fun tearDown() = scope.cancel()

    // ---- the app as installed ------------------------------------------------------------------------------------

    @Test
    fun `a first launch explains the missing permission and has nothing to control`() {
        advance(100)
        compose.onNodeWithText("Bluetooth permission needed").assertExists()
        compose.onNodeWithText("Allow").assertExists()
        compose.onNodeWithText("Nothing to control yet", substring = true).assertExists()
        compose.onNodeWithContentDescription("Power").assertDoesNotExist()
        assertEquals(ConnectionStatus.Disconnected, app.repository.connection.value)
        snapshot("first-launch")
    }

    @Test
    fun `with the permission granted and bluetooth off it offers to turn it on`() {
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        compose.activityRule.scenario.moveToState(Lifecycle.State.STARTED).moveToState(Lifecycle.State.RESUMED)
        advance(100)
        compose.onNodeWithText("Bluetooth is off").assertExists()
        compose.onNodeWithText("Turn on").assertExists()
        assertEquals("no connection attempt while the radio is off", ConnectionStatus.Disconnected, app.repository.connection.value)
        snapshot("bluetooth-off")
    }

    @Test
    fun `demo mode switched on in settings brings the whole app to life`() {
        compose.onNodeWithText("Settings").performClick()
        compose.onNodeWithText("Connect to the jacket to see what it runs.").assertExists()
        compose.onNodeWithText("Demo mode").performScrollTo().assertIsOff().performClick()
        advance(1_000)

        assertTrue(app.repository.demoMode.value)
        assertEquals(ConnectionStatus.Ready, app.repository.connection.value)
        compose.onNodeWithText("Demo mode").assertIsOn()
        compose.onNodeWithText("2.0.0").assertExists() // firmware info arrived from the (fake) jacket
        compose.onNodeWithText("144 + 144 LEDs").assertExists()
        compose.onAllNodes(hasText("900 mA")).assertCountEquals(2) // the selected option and what the jacket reports
        compose.onNodeWithText("About 370 mA of any budget is the jacket's idle draw and makes no light.").assertExists()
        compose.onNodeWithText("NORMAL").assertIsSelected()
        snapshot("settings-demo")

        compose.onNodeWithText("MAX").performScrollTo().performClick()
        advance(300)
        compose.onAllNodes(hasText("2000 mA")).assertCountEquals(2)
        compose.onNodeWithText("MAX").assertIsSelected()

        compose.onNodeWithText("Dev mode").performScrollTo().performClick()
        advance(300)
        compose.onNodeWithText("Dev mode").assertIsOn()
        compose.onNodeWithText("Jacket WiFi").assertExists()
        assertTrue(app.repository.state.value!!.devMode)
        assertEquals(2000, app.repository.state.value!!.budgetMa)

        compose.onNodeWithText("Home").performClick()
        advance(100)
        compose.onNodeWithText("A simulated jacket. Nothing is sent over Bluetooth.").assertExists()
        compose.onNodeWithText("Bluetooth permission needed").assertDoesNotExist()
        for (effect in FakeJacket.EFFECTS) effectTile(effect.name).assertExists()
        effectTile("Cylon").assertIsSelected()
        // The three reactive effects of the firmware say what they react to.
        effectTile("Beat Pulse").assert(hasText("SOUND"))
        effectTile("VU Meter").assert(hasText("SOUND"))
        effectTile("Sparkle").assert(hasText("MOTION"))
        compose.onAllNodes(hasText("SOUND")).assertCountEquals(2)
        compose.onAllNodes(hasText("MOTION")).assertCountEquals(1)
        snapshot("home-demo")

        compose.onNodeWithContentDescription("Power").assertIsOn().performClick()
        advance(300)
        compose.onNodeWithContentDescription("Power").assertIsOff()
        compose.onNodeWithText("The jacket is off").assertExists()
        assertFalse(app.repository.state.value!!.power)
    }

    // ---- the screens against a scripted jacket ----------------------------------------------------------------------

    /** A jacket whose catalogue looks nothing like the v1 firmware's: whatever shows on screen came over the wire. */
    private val effects = listOf(
        EffectInfo(3, ParamFlags(0x00), "Blackout"),
        EffectInfo(9, ParamFlags(0x01), "Plasma"),
        EffectInfo(40, ParamFlags(0x02 or 0x04 or 0x10), "Aurora"),
        EffectInfo(41, ParamFlags(0x08 or 0x20 or 0x40), "Pulse"),
        EffectInfo(42, ParamFlags(0x04 or 0x08), "Drift"),
    )
    private val palettes = listOf(PaletteInfo(0, "Colours"), PaletteInfo(7, "Lava"), PaletteInfo(8, "Glacier"))

    private var jacket = state(effect = 0)

    private val link = ScriptedLink().apply {
        onCommand = { command ->
            when (command) {
                Command.GetInfo -> notify(FakeJacket.INFO)
                is Command.GetCatalog -> {
                    val all = if (command.kind == CatalogKind.EFFECTS) effects else palettes
                    notify(CatalogPage(command.kind, command.start, all.size, all.drop(command.start).take(2)))
                }
                is Command.SetBrightness -> report(jacket.copy(brightness = command.value))
                is Command.SetParam -> report(jacket.copy(segments = jacket.segments.map { it.copy(config = it.config.with(command.param)) }))
                else -> Unit
            }
        }
    }

    private fun report(next: JacketState) {
        jacket = next.copy(rev = jacket.rev + 1)
        link.notify(jacket)
    }

    private fun scripted(effect: Int): JacketRepository {
        val repository = JacketRepository(scope, { link }, demoMode = true)
        compose.activity.setContent { JacketApp(repository, LiveController(scope, repository, LiveFeatures())) }
        advance(100)
        link.status.value = LinkStatus.Connected
        report(state(effect))
        advance(300)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        link.writes.clear()
        return repository
    }

    @Test
    fun `the effect grid is whatever catalogue the jacket sent`() {
        scripted(effect = 9)
        for (effect in effects) effectTile(effect.name).assertExists()
        compose.onAllNodes(isEffectTile).assertCountEquals(effects.size)
        for (effect in FakeJacket.EFFECTS) effectTile(effect.name).assertDoesNotExist()
        effectTile("Plasma").assertIsSelected()
        // Pulse is the only one declaring that it reacts to sound and motion.
        compose.onAllNodes(hasText("SOUND").and(hasText("MOTION"))).assertCountEquals(1)
        effectTile("Pulse").assert(hasText("SOUND").and(hasText("MOTION")))
        snapshot("home-scripted")
    }

    @Test
    fun `only the controls the selected effect declares are shown`() {
        val repository = scripted(effect = 9)
        val all = listOf("Colour 1", "Colour 2", "Palette", "Speed", "Intensity")
        fun assertControls(vararg shown: String) {
            for (name in all) {
                val nodes = compose.onAllNodes(hasText(name))
                if (name in shown) nodes.assertCountEquals(1) else nodes.assertCountEquals(0)
            }
        }
        assertControls("Colour 1")

        effectTile("Aurora").performClick()
        advance(100)
        assertEquals(listOf(Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Effect(40)), true)), link.writes)
        assertEquals("shown before the jacket confirms", 40, repository.state.value!!.segments[0].config.effect)
        scrollTo("Intensity")
        // Aurora declares colour 2 and the palette. With palette 0, which is made of both colours, colour 1 shows too.
        assertControls("Colour 1", "Colour 2", "Palette", "Intensity")
        snapshot("home-aurora")
        scrollTo("Lava")
        compose.onNodeWithText("Lava").performClick()
        advance(100)
        assertControls("Colour 2", "Palette", "Intensity")

        // Drift declares no colour at all, only the palette: both pickers with palette 0, none with another.
        scrollTo("Drift")
        effectTile("Drift").performClick()
        advance(100)
        scrollTo("Speed")
        assertControls("Palette", "Speed")
        scrollTo("Colours")
        compose.onNodeWithText("Colours").performClick()
        advance(100)
        scrollTo("Speed")
        assertControls("Colour 1", "Colour 2", "Palette", "Speed")

        scrollTo("Pulse")
        effectTile("Pulse").performClick()
        advance(100)
        scrollTo("Speed")
        assertControls("Speed")

        scrollTo("Blackout")
        effectTile("Blackout").performClick()
        advance(100)
        scrollTo("This effect has nothing to adjust.")
        assertControls()
    }

    @Test
    fun `choosing a palette and a colour shortcut writes them for every segment`() {
        scripted(effect = 40)
        scrollTo("Lava")
        compose.onNodeWithText("Lava").performClick()
        advance(100)
        assertEquals(listOf(Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Palette(7)), true)), link.writes)

        link.writes.clear()
        compose.onNode(hasClickLabel("Use #FF0000")).performClick()
        advance(100)
        assertEquals(listOf(Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Color2(Rgb(0xFF0000))), true)), link.writes)
    }

    @Test
    fun `dragging brightness streams unacknowledged values and ends with one acknowledged write`() {
        val repository = scripted(effect = 9)
        compose.onNodeWithContentDescription("Brightness").performTouchInput { swipeRight(startX = centerX, endX = right, durationMillis = 400) }
        advance(300)

        val values = link.writes.map { (it.command as Command.SetBrightness).value }
        assertTrue("at least a preview and the final value, got $values", values.size >= 2)
        assertEquals("the drag only went up", values.sorted(), values)
        assertTrue(link.writes.dropLast(1).none { it.withResponse })
        assertTrue(link.writes.last().withResponse)
        assertEquals(255, values.last())
        assertEquals(255, repository.state.value!!.brightness)
        compose.onNodeWithText("100%").assertExists()
    }

    @Test
    fun `losing the jacket greys the controls out but keeps the last state on screen`() {
        scripted(effect = 9)
        link.status.value = LinkStatus.Connecting
        advance(100)
        compose.onNodeWithText("Connecting to the demo jacket…").assertExists()
        effectTile("Plasma").assertIsSelected().assertIsNotEnabled()
        compose.onNodeWithContentDescription("Power").assertIsNotEnabled()
        compose.onNodeWithContentDescription("Brightness").assertIsNotEnabled()
        snapshot("home-reconnecting")
    }

    // ---- presets, zones and the zone selector, against the fake jacket ------------------------------------------------

    private class Demo(val repository: JacketRepository, val jacket: FakeJacket, val link: RecordingLink)

    /** The app on a fake jacket whose traffic is recorded, connected and synced. */
    private fun demo(): Demo {
        val jacket = FakeJacket(scope)
        val link = RecordingLink(jacket)
        val repository = JacketRepository(scope, { link }, demoMode = true)
        compose.activity.setContent { JacketApp(repository, LiveController(scope, repository, LiveFeatures())) }
        advance(1_000)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        link.writes.clear()
        return Demo(repository, jacket, link)
    }

    private fun open(tab: String) {
        compose.onNodeWithText(tab).performClick()
        advance(100)
    }

    /** A button inside the row of preset slot [number] (as shown, so starting at 1). */
    private fun inSlot(number: Int, button: String) =
        compose.onNode(hasText(button).and(hasAnyAncestor(hasContentDescription("Slot $number"))))

    private fun inDialog(text: String) = compose.onNode(hasText(text).and(hasAnyAncestor(isDialog())))

    @Test
    fun `presets are saved, loaded and deleted, and the active one is marked`() {
        val demo = demo()
        open("Presets")
        compose.onAllNodes(hasText("Empty")).assertCountEquals(8)
        compose.onAllNodes(hasText("ACTIVE")).assertCountEquals(0)
        snapshot("presets-empty")

        // Save, with a name longer than the 16 bytes the jacket takes: accents and emoji count for more than one.
        inSlot(3, "Save here").performClick()
        advance(100)
        inSlot(3, "Save").assertIsNotEnabled() // no name yet
        compose.onNode(hasSetTextAction()).performTextInput("Néon ⚡ sunset over the city")
        advance(100)
        compose.onNode(hasSetTextAction()).assert(hasText("Néon ⚡ sunset"))
        compose.onNode(hasSetTextAction()).assert(hasText("16 of 16 bytes", substring = true))
        snapshot("presets-naming")
        inSlot(3, "Save").performClick()
        advance(300)

        assertEquals(listOf(Write(Command.PresetSave(2, "Néon ⚡ sunset"), true)), demo.link.writes)
        compose.onAllNodes(hasText("Empty")).assertCountEquals(7)
        compose.onNodeWithText("Néon ⚡ sunset").assertExists()
        compose.onAllNodes(hasText("ACTIVE")).assertCountEquals(1)
        snapshot("presets")

        // Change the scene: the mark goes. Load: it is back, and so is the brightness.
        demo.repository.commit(Change.Brightness(10))
        advance(300)
        compose.onAllNodes(hasText("ACTIVE")).assertCountEquals(0)
        inSlot(3, "Load").performClick()
        advance(300)
        compose.onAllNodes(hasText("ACTIVE")).assertCountEquals(1)
        assertEquals(128, demo.repository.state.value!!.brightness)
        assertEquals(Write(Command.PresetLoad(2), true), demo.link.writes.last())

        // Overwrite offers the old name; cancelling writes nothing.
        val writes = demo.link.writes.size
        inSlot(3, "Overwrite").performClick()
        advance(100)
        compose.onNode(hasSetTextAction()).assert(hasText("Néon ⚡ sunset"))
        compose.onNode(hasSetTextAction()).assert(hasText("Replaces “Néon ⚡ sunset”", substring = true))
        inSlot(3, "Cancel").performClick()
        advance(100)
        compose.onNode(hasSetTextAction()).assertDoesNotExist()

        // Delete asks first.
        inSlot(3, "Delete").performClick()
        advance(100)
        compose.onNodeWithText("Delete “Néon ⚡ sunset”?").assertExists()
        snapshot("presets-delete-dialog")
        inDialog("Cancel").performClick()
        advance(100)
        assertEquals(writes, demo.link.writes.size)
        compose.onNodeWithText("Néon ⚡ sunset").assertExists()

        inSlot(3, "Delete").performClick()
        advance(100)
        inDialog("Delete").performClick()
        advance(300)
        assertEquals(Write(Command.PresetDelete(2), true), demo.link.writes.last())
        compose.onAllNodes(hasText("Empty")).assertCountEquals(8)
        compose.onAllNodes(hasText("ACTIVE")).assertCountEquals(0)
        assertTrue("the list came with the answers, it was never asked for", demo.link.sent<Command.GetPresets>().isEmpty())
    }

    @Test
    fun `a jacket that cannot store the preset says so on screen`() {
        val demo = demo()
        demo.jacket.storageFails = true
        open("Presets")
        inSlot(1, "Save here").performClick()
        advance(100)
        compose.onNode(hasSetTextAction()).performTextInput("Rave")
        inSlot(1, "Save").performClick()
        advance(300)

        compose.onNodeWithText("The jacket could not save the preset: its storage failed.").assertExists()
        compose.onAllNodes(hasText("Empty")).assertCountEquals(8)
        snapshot("presets-error")

        // The same goes for an empty slot, whichever screen is up when the answer comes.
        demo.jacket.storageFails = false
        demo.repository.loadPreset(5)
        advance(300)
        compose.onNodeWithText("That preset slot is empty.").assertExists()
    }

    @Test
    fun `zones are edited as a draft and reach the jacket once, on apply`() {
        val demo = demo()
        open("Zones")
        compose.onNodeWithContentDescription("Left strip").assertExists()
        compose.onNodeWithContentDescription("Right strip").assertExists()
        fun assertRange(text: String) = compose.onNodeWithContentDescription("Zone range").assert(hasText(text))
        assertRange("0–143 · 144 LEDs")
        compose.onNodeWithText("Apply").assertDoesNotExist()
        snapshot("zones")

        compose.onNodeWithText("Left + Right").performClick()
        advance(100)
        compose.onNodeWithText("Not on the jacket yet.").assertExists()
        compose.onNodeWithContentDescription("Zone 2").assertExists()
        assertEquals("the jacket lights what is being edited", Command.Identify(1, 0, 144, 3), demo.jacket.identified)

        // A look at another tab does not throw the draft away.
        open("Home")
        compose.onNodeWithContentDescription("Zone 2").assertDoesNotExist() // the jacket still has one zone
        open("Zones")
        compose.onNodeWithText("Not on the jacket yet.").assertExists()

        compose.onNodeWithContentDescription("Zone 2").performClick()
        repeat(3) { compose.onNodeWithContentDescription("Last LED down").performClick() }
        repeat(2) { compose.onNodeWithContentDescription("First LED up").performClick() }
        advance(100)
        assertRange("2–140 · 139 LEDs")
        assertEquals(Command.Identify(2, 2, 139, 3), demo.jacket.identified)
        compose.onNodeWithText("Reversed").performScrollTo().assertIsOff().performClick()
        compose.onNodeWithText("Reversed").assertIsOn()

        compose.onNodeWithContentDescription("Add a zone").performClick()
        advance(100)
        compose.onNodeWithContentDescription("Zone 3").assertIsSelected()
        assertRange("141–143 · 3 LEDs") // the longer of the two stretches zone 2 left free on the right strip
        compose.onNodeWithText("Both").performScrollTo().performClick()
        advance(100)
        snapshot("zones-editing")

        assertTrue("nothing but identify has been sent so far", demo.link.writes.all { it.command is Command.Identify })
        assertEquals(1, demo.repository.state.value!!.segments.size)

        compose.onNodeWithText("Apply").performScrollTo().performClick()
        advance(300)
        val expected = listOf(SegmentGeometry(1, 0, 144, 0), SegmentGeometry(2, 2, 139, 1), SegmentGeometry(3, 141, 3, 0))
        assertEquals(listOf<Command>(Command.SetSegments(expected)), demo.link.writes.map { it.command }.filter { it !is Command.Identify })
        assertEquals(expected, demo.repository.state.value!!.segments.map { it.geometry })
        compose.onNodeWithText("Not on the jacket yet.").assertDoesNotExist()
        snapshot("zones-applied")

        // Remove is a draft too, and Discard brings back what the jacket has.
        compose.onNodeWithText("Remove").performScrollTo().performClick()
        advance(100)
        compose.onNodeWithContentDescription("Zone 3").assertDoesNotExist()
        compose.onNodeWithText("Discard").performScrollTo().performClick()
        advance(100)
        compose.onNodeWithContentDescription("Zone 3").assertExists()
        assertEquals(1, demo.link.sent<Command.SetSegments>().size)

        compose.onNodeWithContentDescription("Zone 3").performClick()
        compose.onNodeWithText("Identify").performScrollTo().performClick()
        advance(100)
        assertEquals(Command.Identify(3, 141, 3, 6), demo.jacket.identified)
    }

    @Test
    fun `dragging an end of the selected zone on its strip resizes it in whole LEDs`() {
        val demo = demo()
        open("Zones")
        compose.onNodeWithContentDescription("Left strip").performTouchInput {
            swipe(start = Offset(width - 2f, centerY), end = Offset(centerX, centerY), durationMillis = 300)
        }
        advance(300)

        compose.onNodeWithContentDescription("Zone range").assert(hasText("0–71 · 72 LEDs"))
        assertEquals(Command.Identify(3, 0, 72, 3), demo.jacket.identified)
        assertTrue("the drag itself sends no layout", demo.link.writes.all { it.command is Command.Identify })
        assertTrue("the light follows unacknowledged", demo.link.writes.dropLast(1).none { it.withResponse })
        assertTrue("and the last word is acknowledged", demo.link.writes.last().withResponse)

        // A second zone takes the free half; a tap on the strip picks whichever zone is under the finger.
        compose.onNodeWithContentDescription("Add a zone").performClick()
        advance(100)
        compose.onNodeWithContentDescription("Zone range").assert(hasText("72–143 · 72 LEDs"))
        compose.onNodeWithContentDescription("Right strip").performTouchInput { click(Offset(width * 0.1f, centerY)) }
        advance(100)
        compose.onNodeWithContentDescription("Zone 1").assertIsSelected()
        compose.onNodeWithContentDescription("Left strip").performTouchInput { click(Offset(width * 0.9f, centerY)) }
        advance(100)
        compose.onNodeWithContentDescription("Zone 2").assertIsSelected()

        // The start handle of zone 2, dragged left past the strip's beginning, stops at LED 0.
        compose.onNodeWithContentDescription("Left strip").performTouchInput {
            swipe(start = Offset(centerX, centerY), end = Offset(-50f, centerY), durationMillis = 300)
        }
        advance(300)
        compose.onNodeWithContentDescription("Zone range").assert(hasText("0–143 · 144 LEDs"))
    }

    @Test
    fun `the jacket can hold eight zones and the screen stops there`() {
        demo()
        open("Zones")
        repeat(7) { compose.onNodeWithContentDescription("Add a zone").performClick() }
        advance(100)
        compose.onNodeWithContentDescription("Zone 8").assertIsSelected()
        compose.onNodeWithContentDescription("Add a zone").assertIsNotEnabled()
        compose.onNodeWithText("Apply").assertExists()
        snapshot("zones-eight")
    }

    @Test
    fun `home edits every zone by default and one zone when it is picked`() {
        val demo = demo()
        compose.onNodeWithContentDescription("Zone 1").assertDoesNotExist() // one zone: nothing to choose

        val cylon = demo.repository.state.value!!.segments[0].config
        demo.repository.applyLayout(listOf(Segment(SegmentGeometry(1, 0, 144, 0), cylon), Segment(SegmentGeometry(2, 0, 144, 0), cylon)))
        advance(300)
        demo.link.writes.clear()
        compose.onNode(isSelectable().and(hasText("All"))).assertIsSelected()

        effectTile("Rainbow").performClick()
        advance(300)
        assertEquals(listOf(Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Effect(2)), true)), demo.link.writes)
        assertEquals(listOf(2, 2), demo.repository.state.value!!.segments.map { it.config.effect })

        compose.onNodeWithContentDescription("Zone 2").performClick()
        effectTile("Solid").performClick()
        advance(300)
        assertEquals(Write(Command.SetParam(1, Param.Effect(0)), true), demo.link.writes.last())
        assertEquals(listOf(2, 0), demo.repository.state.value!!.segments.map { it.config.effect })
        effectTile("Solid").assertIsSelected()
        snapshot("home-zones")

        // The parameter controls follow the pick too, for a tap and for a drag alike.
        demo.link.writes.clear()
        scrollTo("Colour 1")
        compose.onNode(hasClickLabel("Use #FF0000")).performClick()
        advance(300)
        assertEquals(listOf(Write(Command.SetParam(1, Param.Color1(Rgb(0xFF0000))), true)), demo.link.writes)
        // The thumb sits at full saturation, its centre half a thumb in from the right end.
        compose.onNodeWithContentDescription("Colour 1 saturation").performTouchInput { swipeLeft(startX = right - 11.dp.toPx(), endX = centerX, durationMillis = 300) }
        advance(300)
        assertTrue("a preview and the final value after the tap, got ${demo.link.writes}", demo.link.writes.size >= 3)
        assertTrue("every write went to zone 2 only", demo.link.sent<Command.SetParam>().all { it.segment == 1 && it.param is Param.Color1 })
        assertEquals(Rgb(0x00FFCC), demo.repository.state.value!!.segments[0].config.color1)
        scrollTo("Zone 1")

        compose.onNodeWithContentDescription("Zone 1").performClick()
        advance(100)
        effectTile("Rainbow").assertIsSelected() // the controls now show zone 1

        open("Zones")
        open("Home")
        compose.onNodeWithContentDescription("Zone 1").assertIsSelected() // the pick survives a look at another tab

        // The layout shrinks under the selection: back to all zones, showing the one that is left.
        compose.onNodeWithContentDescription("Zone 2").performClick()
        demo.repository.applyLayout(demo.repository.state.value!!.segments.take(1))
        advance(300)
        compose.onNodeWithContentDescription("Zone 2").assertDoesNotExist()
        effectTile("Rainbow").assertIsSelected()
        demo.link.writes.clear()
        effectTile("Comet").performClick()
        advance(300)
        assertEquals(listOf(Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Effect(1)), true)), demo.link.writes)
    }

    @Test
    fun `the real catalogue shows both colours for a palette effect on palette zero`() {
        demo() // boots on Cylon (palette, speed, intensity) with palette 0
        effectTile("Cylon").assertIsSelected()
        scrollTo("Intensity")
        for (control in listOf("Colour 1", "Colour 2", "Palette", "Speed", "Intensity")) compose.onAllNodes(hasText(control)).assertCountEquals(1)
        scrollTo("Cyberpunk")
        compose.onNodeWithText("Cyberpunk").performClick()
        advance(300)
        scrollTo("Intensity")
        compose.onAllNodes(hasText("Colour 1")).assertCountEquals(0)
        compose.onAllNodes(hasText("Colour 2")).assertCountEquals(0)
    }

    // ---- live mode ---------------------------------------------------------------------------------------------------

    /** Answers the permission dialog that is up: [granted] are allowed, whatever else was asked for is denied. */
    private fun answerPermissions(vararg granted: String) {
        val request = shadowOf(compose.activity).lastRequestedPermission
        shadowOf(app).grantPermissions(*granted)
        // What the system does when its dialog closes, which Robolectric leaves to the test.
        ReflectionHelpers.setField(Activity::class.java, compose.activity, "mHasCurrentPermissionsRequest", false)
        compose.activity.onRequestPermissionsResult(
            request.requestCode,
            request.requestedPermissions,
            request.requestedPermissions.map { if (it in granted) PackageManager.PERMISSION_GRANTED else PackageManager.PERMISSION_DENIED }.toIntArray(),
        )
        advance(100)
    }

    @Test
    fun `live mode runs in its foreground service, survives the background and stops from the notification`() {
        val sensors = app.getSystemService(SensorManager::class.java)
        val accelerometer = ShadowSensor.newInstance(Sensor.TYPE_ACCELEROMETER)
        shadowOf(sensors).addSensor(accelerometer)
        advance(100)
        answerPermissions(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT) // the first-launch dialog

        open("Live")
        compose.onNodeWithText("Jacket not connected").assertExists()
        compose.onNodeWithText("Start").assertIsNotEnabled()
        snapshot("live-not-connected")

        open("Settings")
        compose.onNodeWithText("Demo mode").performScrollTo().performClick()
        advance(1_000)
        open("Live")
        compose.onNodeWithText("Live mode is off").assertExists()
        compose.onNodeWithText("Microphone permission needed").assertExists()
        compose.onNodeWithText("Notifications are off").assertExists()
        compose.onNodeWithText("It is never recorded and never leaves the phone", substring = true).assertExists()
        compose.onNodeWithText("– BPM").assertExists()
        snapshot("live-off")

        // Start asks for what is missing first, and starts nothing before the answer.
        compose.onNodeWithText("Start").assertIsEnabled().performClick()
        advance(100)
        assertEquals(
            listOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS),
            shadowOf(compose.activity).lastRequestedPermission.requestedPermissions.toList(),
        )
        assertNull(shadowOf(app).peekNextStartedService())
        assertFalse(app.live.running)

        // Notifications allowed, the microphone refused: it runs on motion alone and says what is missing.
        answerPermissions(Manifest.permission.POST_NOTIFICATIONS)
        compose.onNodeWithText("Microphone permission denied").assertExists()
        compose.onNodeWithText("Notifications are off").assertDoesNotExist()
        val start = shadowOf(app).nextStartedService
        assertEquals(LiveService::class.java.name, start.component!!.className)
        val service = Robolectric.buildService(LiveService::class.java, start).create().startCommand(0, 1)
        advance(100)

        assertTrue(app.live.running)
        assertEquals(
            "no microphone type without the permission; the Bluetooth one is allowed",
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            service.get().foregroundServiceType,
        )
        val notification = shadowOf(service.get()).lastForegroundNotification
        assertTrue(notification.flags and Notification.FLAG_ONGOING_EVENT != 0)
        assertEquals("Stop", notification.actions.single().title)
        assertTrue(notification.extras.getCharSequence(Notification.EXTRA_TEXT)!!.contains("Nothing is recorded"))
        assertEquals(1, shadowOf(sensors).listeners.size)
        assertTrue("awake with the screen off", ShadowPowerManager.getLatestWakeLock().isHeld)

        // The phone on its left edge, being shaken: that is what goes out, to the fake jacket's live characteristic.
        val sensingFrom = SystemClock.elapsedRealtime()
        val sentBeforeSensing = app.live.framesSent
        var shake = 0
        fun sense() {
            val wobble = 12f * sin(shake++ * 0.5f)
            val event = SensorEventBuilder.newBuilder().setSensor(accelerometer)
                .setValues(floatArrayOf(9.81f + wobble, 0f, 0f)).setTimestamp(SystemClock.elapsedRealtimeNanos()).build()
            shadowOf(sensors).sendSensorEventToListeners(event)
        }
        repeat(150) {
            sense()
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(20))
        }
        advance(20)
        assertTrue("the jacket's live flag", app.repository.state.value!!.liveInput)
        assertFrameRate(app.live.framesSent - sentBeforeSensing, SystemClock.elapsedRealtime() - sensingFrom)
        with(app.live.meters) {
            assertTrue(motionValid)
            assertFalse("the microphone is not allowed, so there is no sound", audioValid)
            assertTrue("tilt $tiltX", tiltX >= 120)
            assertEquals(0, tiltY)
            assertTrue("motion $motion", motion >= 150)
        }
        compose.onNodeWithText("Streaming to the demo jacket").assertExists()
        compose.onNodeWithText("Demo mode: the frames go to the simulated jacket, nothing is sent over Bluetooth.").assertExists()
        // The fake jacket boots on Cylon, which reacts to nothing; Sparkle reacts to motion.
        compose.onNodeWithText("No zone is playing a reactive effect", substring = true).assertExists()
        snapshot("live-demo-motion")
        open("Home")
        scrollTo("Sparkle")
        effectTile("Sparkle").assert(hasText("MOTION")).performClick()
        advance(300)
        open("Live")
        compose.onNodeWithText("Streaming to the demo jacket").assertExists()
        compose.onNodeWithText("No zone is playing a reactive effect", substring = true).assertDoesNotExist()

        // The app leaves the screen for five minutes: the connection and the stream stay.
        compose.activityRule.scenario.moveToState(Lifecycle.State.CREATED)
        val sentBefore = app.live.framesSent
        val hiddenFrom = SystemClock.elapsedRealtime()
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMinutes(5))
        assertEquals(ConnectionStatus.Ready, app.repository.connection.value)
        assertTrue(app.live.running)
        assertTrue(SystemClock.elapsedRealtime() - hiddenFrom >= 300_000)
        assertFrameRate(app.live.framesSent - sentBefore, SystemClock.elapsedRealtime() - hiddenFrom)
        assertTrue(app.repository.state.value!!.liveInput)
        assertFalse("the accelerometer went quiet, so its flag did too", app.live.meters.motionValid)

        // Stop in the notification: the service goes, and 20 s later so does the connection nobody needs any more.
        val stop = shadowOf(shadowOf(service.get()).lastForegroundNotification.actions.single().actionIntent).savedIntent
        assertEquals(LiveService.ACTION_STOP, stop.action)
        service.get().onStartCommand(stop, 0, 2)
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(100))
        assertFalse(app.live.running)
        assertTrue(shadowOf(service.get()).isStoppedBySelf)
        assertTrue(shadowOf(service.get()).isForegroundStopped)
        assertTrue(shadowOf(sensors).listeners.isEmpty())
        assertFalse(ShadowPowerManager.getLatestWakeLock().isHeld)
        val sentAtStop = app.live.framesSent
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(19))
        assertEquals(ConnectionStatus.Ready, app.repository.connection.value)
        assertFalse("the jacket's own timeout cleared the flag", app.repository.state.value!!.liveInput)
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(2))
        assertEquals(ConnectionStatus.Disconnected, app.repository.connection.value)
        assertEquals(sentAtStop, app.live.framesSent)

        // Back on screen: connected again, and live mode is off, as the notification left it.
        compose.activityRule.scenario.moveToState(Lifecycle.State.RESUMED)
        advance(1_000)
        assertEquals(ConnectionStatus.Ready, app.repository.connection.value)
        compose.onNodeWithText("Live mode is off").assertExists()
        compose.onNodeWithText("Start").assertIsEnabled()
    }

    @Test
    fun `the service does not outlive live mode, however it ends, and settings survive the app`() {
        advance(100)
        answerPermissions(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        app.repository.setDemoMode(true)
        app.repository.connect()
        advance(1_000)
        app.live.update { it.copy(sound = false, sensitivity = 80, latencyOffsetMs = -45) }

        // Stopped from the screen (or by the controller itself): the service follows.
        val service = Robolectric.buildService(LiveService::class.java).create().startCommand(0, 1)
        advance(200)
        assertTrue(app.live.running)
        assertTrue(app.repository.state.value!!.liveInput)
        app.live.stop(LiveNotice.IdleTimeout)
        advance(100)
        assertTrue(shadowOf(service.get()).isStoppedBySelf)
        assertTrue(shadowOf(service.get()).isForegroundStopped)

        // Taken away by Android: live mode ends with it.
        val second = Robolectric.buildService(LiveService::class.java).create().startCommand(0, 1)
        advance(200)
        assertTrue(app.live.running)
        second.destroy()
        assertFalse(app.live.running)

        // Refused by Android: nothing runs and the screen says so.
        val third = Robolectric.buildService(LiveService::class.java).create()
        shadowOf(third.get()).setThrowInStartForeground(SecurityException("not while in the background"))
        third.startCommand(0, 1)
        advance(100)
        assertFalse(app.live.running)
        assertEquals(LiveNotice.StartRefused, app.live.notice.value)
        assertTrue(shadowOf(third.get()).isStoppedBySelf)
        open("Live")
        compose.onNodeWithText("Android did not let live mode start. Try again with the app open.").assertExists()

        // With nothing to sense the service has nothing to do.
        app.live.update { it.copy(motion = false) }
        val fourth = Robolectric.buildService(LiveService::class.java).create().startCommand(0, 1)
        advance(100)
        assertFalse(app.live.running)
        assertTrue(shadowOf(fourth.get()).isStoppedBySelf)

        // What was chosen is in the preferences file, where the next process finds it.
        val stored = app.getSharedPreferences("settings", Context.MODE_PRIVATE)
        assertEquals(false, stored.getBoolean("live_sound", true))
        assertEquals(false, stored.getBoolean("live_motion", true))
        assertEquals(80, stored.getInt("live_sensitivity", -1))
        assertEquals(-45, stored.getInt("live_latency_offset_ms", 0))
    }

    @Test
    fun `which foreground types the service takes follows the settings and the permissions`() {
        fun types(settings: LiveSettings) = LiveService.foregroundTypes(app, settings)
        val microphone = ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        val device = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        assertEquals("nothing granted: no type may be claimed", 0, types(LiveSettings()))
        shadowOf(app).grantPermissions(Manifest.permission.RECORD_AUDIO)
        assertEquals(microphone, types(LiveSettings()))
        assertEquals("the microphone type only while it listens", 0, types(LiveSettings(sound = false)))
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT)
        assertEquals(microphone or device, types(LiveSettings()))
        assertEquals(device, types(LiveSettings(sound = false)))
    }

    /** The Live screen on a scripted jacket, with a controller of its own and analysers the test plays. */
    private inner class LiveBench(effect: Int) {
        val repository = JacketRepository(scope, { link }, demoMode = false)
        val features = LiveFeatures()
        val live = LiveController(scope, repository, features, nanoTime = SystemClock::elapsedRealtimeNanos)
        var level = 180
        var tiltX = 60

        init {
            shadowOf(app).grantPermissions(Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS)
            compose.activity.setContent { JacketApp(repository, live, startLive = { live.start() }) }
            advance(100)
            link.status.value = LinkStatus.Connected
            report(state(effect))
            advance(300)
            assertEquals(ConnectionStatus.Ready, repository.connection.value)
            // A microphone that hears 128 BPM and an accelerometer, both delivering every 20 ms.
            scope.launch {
                while (true) {
                    val now = SystemClock.elapsedRealtimeNanos()
                    val beat = 60e9 / 128
                    features.audio(now, level, 230, 120, 60, beatValid = true, bpm = 128f, beatPhase = (now % beat / beat).toFloat(), beatPeriodSeconds = 60f / 128)
                    features.motion(now, 140, tiltX, -90)
                    delay(20)
                }
            }
            open("Live")
        }
    }

    @Test
    fun `the live screen starts and stops the stream and shows what goes out`() {
        val bench = LiveBench(effect = 41) // Pulse, which reacts to sound and motion
        compose.onNodeWithText("Live mode is off").assertExists()
        compose.onNodeWithText("Microphone permission needed").assertDoesNotExist()
        compose.onNodeWithText("Notifications are off").assertDoesNotExist()
        assertTrue(link.live.isEmpty())

        val startedAt = SystemClock.elapsedRealtime()
        compose.onNodeWithText("Start").performClick()
        advance(500)
        assertTrue(bench.live.running)
        assertFrameRate(link.live.size.toLong(), SystemClock.elapsedRealtime() - startedAt)
        compose.onNodeWithText("Streaming to the jacket").assertExists()
        compose.onNodeWithText("Sending 30 times a second. Keeps going with the screen off.").assertExists()
        compose.onNodeWithContentDescription("Tempo").assert(hasText("128 BPM"))
        compose.onNodeWithText("No zone is playing a reactive effect", substring = true).assertDoesNotExist()
        assertEquals("numbered from the first", link.live.indices.toList(), link.live.map { it.seq })
        with(link.live.last()) {
            assertEquals(7, flags)
            assertEquals(listOf(180, 230, 120, 60, 1280, 140, 60, -90), listOf(level, bass, mid, treble, bpmX10, motion, tiltX, tiltY))
        }

        // The jacket confirms with its live flag, as the real one does.
        report(jacket.copy(flags = JacketState.FLAG_LIVE))
        advance(200)
        compose.onNodeWithText("The jacket is receiving, 30 times a second. Keeps going with the screen off.").assertExists()
        snapshot("live-streaming")

        // An effect that reacts to nothing: say so, live mode would look broken otherwise.
        report(jacket.copy(segments = jacket.segments.map { it.copy(config = it.config.copy(effect = 9)) }))
        advance(200)
        compose.onNodeWithText("No zone is playing a reactive effect. Pick one marked SOUND or MOTION on Home to see the jacket react.").assertExists()
        snapshot("live-no-reactive-effect")

        // The jacket walks away: still listening, nothing sent. And comes back.
        link.status.value = LinkStatus.Connecting
        advance(300)
        compose.onNodeWithText("Waiting for the jacket…").assertExists()
        compose.onNodeWithText("Stop").assertExists()
        val sent = link.live.size
        advance(500)
        assertEquals(sent, link.live.size)
        compose.onNodeWithContentDescription("Tempo").assert(hasText("128 BPM"))
        snapshot("live-waiting")
        link.status.value = LinkStatus.Connected
        report(jacket)
        advance(500)
        compose.onNodeWithText("Streaming to the jacket").assertExists()
        assertTrue(link.live.size > sent)

        compose.onNodeWithText("Stop").performClick()
        advance(100)
        assertFalse(bench.live.running)
        val stopped = link.live.size
        advance(1_000)
        assertEquals("nothing after Stop", stopped, link.live.size)
        compose.onNodeWithText("Live mode is off").assertExists()
        compose.onNodeWithContentDescription("Tempo").assert(hasText("– BPM"))

        // When it stopped by itself, the screen says why.
        bench.live.start()
        bench.live.stop(LiveNotice.IdleTimeout)
        advance(100)
        compose.onNodeWithText("Live mode stopped by itself: the jacket was away for 5 minutes.").assertExists()
        snapshot("live-idle-timeout")
    }

    @Test
    fun `the meters move without the screen being composed again`() {
        val bench = LiveBench(effect = 41)
        compose.onNodeWithText("Start").performClick()
        advance(500)

        fun barEndsAt(): Int {
            val image = compose.onNodeWithContentDescription("Level meter").captureToImage().asAndroidBitmap()
            val row = image.height / 2
            return (0 until image.width).lastOrNull { image.getPixel(it, row) == android.graphics.Color.rgb(0x00, 0xFF, 0xCC) } ?: -1
        }
        val width = compose.onNodeWithContentDescription("Level meter").fetchSemanticsNode().size.width
        val before = barEndsAt()
        assertTrue("180 of 255 is 71 % of $width px: $before", abs(before - width * 180 / 255) <= 3)

        val composed = LiveScreenStats.compositions
        bench.level = 51
        bench.tiltX = -100
        advance(2_000) // sixty frames, every one with another beat phase
        val after = barEndsAt()
        assertTrue("51 of 255 is 20 % of $width px: $after", abs(after - width * 51 / 255) <= 3)
        assertEquals(51, link.live.last().level)
        assertEquals("sixty new frames were drawn, none was composed", composed, LiveScreenStats.compositions)

        // A new tempo is text, so that one line is composed again; once, not once per frame.
        val phases = link.live.takeLast(30).map { it.beatPhase }.distinct().size
        assertTrue("the beat light had $phases different phases to show", phases >= 20)
    }

    @Test
    fun `sound, motion, sensitivity and beat timing are set on the live screen`() {
        val bench = LiveBench(effect = 41)
        assertEquals(LiveSettings(), bench.live.settings.value)

        centre(hasContentDescription("Beat timing"))
        compose.onNodeWithContentDescription("Beat timing").performTouchInput { swipeRight(startX = centerX, endX = right + 50f, durationMillis = 300) }
        advance(100)
        assertEquals(300, bench.live.settings.value.latencyOffsetMs)
        compose.onNodeWithText("+300 ms").assertExists()
        compose.onNodeWithContentDescription("Beat timing").performTouchInput { swipeLeft(startX = right - 11.dp.toPx(), endX = left - 50f, durationMillis = 300) }
        advance(100)
        assertEquals(-200, bench.live.settings.value.latencyOffsetMs)
        compose.onNodeWithText("−200 ms").assertExists()
        compose.onNodeWithText("slide until the flash lands on the beat", substring = true).assertExists()
        // A tap in the middle of the track: +50 ms, in steps of 5.
        compose.onNodeWithContentDescription("Beat timing").performTouchInput { click(center) }
        advance(100)
        assertEquals(50, bench.live.settings.value.latencyOffsetMs)

        centre(hasContentDescription("Sensitivity"))
        compose.onNodeWithContentDescription("Sensitivity").performTouchInput { swipeRight(startX = centerX, endX = right + 50f, durationMillis = 300) }
        advance(100)
        assertEquals(100, bench.live.settings.value.sensitivity)
        compose.onNodeWithText("100%").assertExists()
        snapshot("live-tuning")

        // While streaming, the offset moves the phase that goes out, at once.
        compose.onNodeWithText("Start").performScrollTo().performClick()
        advance(300)
        fun phaseLead(frame: com.ouassimf.jacket.protocol.LiveFrame): Int {
            // The fake microphone's phase is the clock modulo one beat, and this early t_ms still is that clock.
            assertEquals(SystemClock.elapsedRealtime().toInt() shr 16, 0)
            val beat = 60_000.0 / 128
            val heard = (frame.tMs % beat / beat * 256).toInt()
            return (frame.beatPhase - heard + 256) % 256
        }
        val lead = phaseLead(link.live.last())
        assertTrue("50 ms of a 469 ms beat is 27 of 256: $lead", lead in 25..29)

        compose.onNodeWithText("Sound").performScrollTo().assertIsOn().performClick()
        advance(100)
        assertFalse(bench.live.settings.value.sound)
        assertEquals(com.ouassimf.jacket.protocol.LiveFrame.FLAG_MOTION, link.live.last().flags)
        compose.onNodeWithText("Motion").performScrollTo().assertIsOn().performClick()
        advance(100)
        assertFalse("with nothing left to sense it stops", bench.live.running)
        compose.onNodeWithText("Switch on Sound or Motion below to have something to send.").assertExists()
        compose.onNodeWithText("Start").assertIsNotEnabled()
        snapshot("live-nothing-to-send")
    }

    // ---- helpers ---------------------------------------------------------------------------------------------------

    /** Effect tiles are the radio buttons of Home (the bottom bar's tabs are selectable too, but as tabs). */
    private val isEffectTile = isSelectable().and(SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.RadioButton))

    private fun effectTile(name: String) = compose.onNode(isEffectTile.and(hasText(name)))

    private fun hasClickLabel(label: String) = SemanticsMatcher("click label is '$label'") {
        it.config.getOrNull(SemanticsActions.OnClick)?.label == label
    }

    /**
     * Home is a lazy grid: what is off screen does not exist until its item is scrolled to, and a tall item can
     * be on screen with the wanted control still below the fold. So: to the item first, then centre the node.
     */
    private fun scrollTo(text: String) {
        val wanted = hasText(text).or(hasContentDescription(text))
        val grid = compose.onAllNodes(hasScrollToNodeAction()).onFirst()
        grid.performScrollToNode(wanted)
        advance(50)
        val node = compose.onAllNodes(wanted).onFirst().fetchSemanticsNode()
        val offset = node.positionInRoot.y + node.size.height / 2 - grid.fetchSemanticsNode().boundsInRoot.center.y
        grid.performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, offset) }
        advance(50)
    }

    /**
     * Brings a node of a plainly scrolling screen to the middle of it. `performScrollTo` alone stops as soon as
     * the node is inside the scrolling area, which reaches under the bottom bar: a touch there hits the bar.
     */
    private fun centre(wanted: SemanticsMatcher) {
        compose.onNode(wanted).performScrollTo()
        val scroller = compose.onAllNodes(hasScrollAction()).onFirst()
        val node = compose.onNode(wanted).fetchSemanticsNode()
        val offset = node.positionInRoot.y + node.size.height / 2 - scroller.fetchSemanticsNode().boundsInRoot.center.y
        scroller.performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, offset) }
        advance(50)
    }

    /** Thirty frames a second, measured against the clock as it really moved (drawing moves it too, here). */
    private fun assertFrameRate(frames: Long, elapsedMillis: Long) {
        assertTrue("$frames frames in $elapsedMillis ms", abs(frames - elapsedMillis * 30 / 1000.0) <= 3)
    }

    /**
     * Lets [millis] pass, then lets Compose settle. Coroutine delays of the app live on the main looper, which
     * Robolectric only moves when told to; Compose's own clock moves by itself.
     */
    private fun advance(millis: Long) {
        compose.waitForIdle() // effects of the last interaction (a connect, say) start before time moves
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(millis))
        compose.waitForIdle()
    }

    private fun snapshot(name: String) {
        val dir = System.getProperty("jacket.screenshots") ?: return
        val file = File(dir, "$name.png").apply { parentFile!!.mkdirs() }
        file.outputStream().use { compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    private fun state(effect: Int) = JacketState(
        proto = Protocol.VERSION,
        rev = 1,
        power = true,
        brightness = 128,
        budgetMa = 900,
        flags = 0,
        preset = Protocol.NO_PRESET,
        segments = listOf(
            Segment(SegmentGeometry(3, 0, 144, 0), EffectConfig(effect, 0, 128, 128, Rgb(0x00FFCC), Rgb(0xFF00FF))),
        ),
    )
}
