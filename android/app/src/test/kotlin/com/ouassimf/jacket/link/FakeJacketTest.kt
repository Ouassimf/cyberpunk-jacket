package com.ouassimf.jacket.link

import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.DeviceInfo
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.ErrorCode
import com.ouassimf.jacket.protocol.ErrorReply
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.LiveFrame
import com.ouassimf.jacket.protocol.Notification
import com.ouassimf.jacket.protocol.Opcode
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.PresetInfo
import com.ouassimf.jacket.protocol.PresetList
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.SegmentGeometry
import com.ouassimf.jacket.protocol.hexToBytes
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException

/** The fake jacket against PROTOCOL.md: each test is one of the document's rules. */
@OptIn(ExperimentalCoroutinesApi::class)
class FakeJacketTest {

    /** A connected fake jacket and everything it notified, decoded. */
    private class Bench(private val scope: TestScope, maxPageEntries: Int) {
        fun runCurrent() = scope.runCurrent()

        val jacket = FakeJacket(scope.backgroundScope, maxPageEntries = maxPageEntries)
        val received = ArrayList<Notification>()

        init {
            scope.backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) {
                jacket.frames.collect { received += Codec.decodeNotification(it) }
            }
        }

        val states get() = received.filterIsInstance<JacketState>()
        val state get() = states.last()
        val errors get() = received.filterIsInstance<ErrorReply>()
        val presetLists get() = received.filterIsInstance<PresetList>()

        suspend fun send(command: Command) = raw(Codec.encode(command))

        /** Writes a frame and lets the replies that are due right away arrive. */
        suspend fun raw(frame: ByteArray) {
            jacket.send(frame, withResponse = true)
            runCurrent()
        }

        /** Sends, then waits out the STATE coalescing, so [state] is what the command left behind. */
        suspend fun apply(vararg commands: Command) {
            for (command in commands) send(command)
            delay(200)
        }
    }

    private fun bench(maxPageEntries: Int = Int.MAX_VALUE, block: suspend Bench.() -> Unit) = runTest {
        val bench = Bench(this, maxPageEntries)
        bench.jacket.connect()
        delay(1_000)
        bench.block()
    }

    // ---- link behaviour ----------------------------------------------------------------------------------------

    @Test
    fun `it boots powered on with one segment over both strips and delivers that on connect`() = runTest {
        val bench = Bench(this, Int.MAX_VALUE)
        assertEquals(LinkStatus.Disconnected, bench.jacket.status.value)

        bench.jacket.connect()
        runCurrent()
        assertEquals(LinkStatus.Connecting, bench.jacket.status.value)
        delay(1_000)

        assertEquals(LinkStatus.Connected, bench.jacket.status.value)
        assertEquals(1, bench.states.size)
        with(bench.state) {
            assertEquals(Protocol.VERSION, proto)
            assertEquals("rev starts at 0 at boot", 0, rev)
            assertTrue("power is not stored: the jacket always boots on", power)
            assertEquals(Protocol.BUDGET_DEFAULT_MA, budgetMa)
            assertEquals(Protocol.NO_PRESET, preset)
            assertEquals(listOf(SegmentGeometry(3, 0, 144, 0)), segments.map { it.geometry })
        }
    }

    @Test
    fun `writing while disconnected fails`() = runTest {
        val jacket = FakeJacket(backgroundScope)
        assertTrue(runCatching { jacket.send(Codec.encode(Command.GetInfo), withResponse = true) }.exceptionOrNull() is IOException)
    }

    @Test
    fun `a dropped link reconnects and delivers the state again`() = bench {
        apply(Command.SetBrightness(5))
        val before = states.size

        jacket.simulateDrop()
        runCurrent()
        assertEquals(LinkStatus.Connecting, jacket.status.value)
        assertTrue(runCatching { send(Command.GetInfo) }.exceptionOrNull() is IOException)
        delay(1_000)

        assertEquals(LinkStatus.Connected, jacket.status.value)
        assertEquals(before + 1, states.size)
        assertEquals(5, state.brightness)
    }

    @Test
    fun `state notifications are coalesced to about ten per second`() = bench {
        val before = states.size
        repeat(100) {
            send(Command.SetBrightness(it))
            delay(10)
        }
        delay(500)
        val during = states.size - before
        assertTrue("$during notifications in one second", during in 9..12)
        assertEquals(99, state.brightness)
    }

    // ---- info and catalogue ------------------------------------------------------------------------------------

    @Test
    fun `info describes two strips and the catalogue sizes`() = bench {
        send(Command.GetInfo)
        val info = received.last() as DeviceInfo
        assertEquals(listOf(144, 144), info.leds)
        assertEquals(8, info.maxSegments)
        assertEquals(8, info.presetSlots)
        assertEquals(10, info.effectCount)
        assertEquals(6, info.paletteCount)
    }

    @Test
    fun `the catalogue is the published v1 table`() {
        assertEquals(
            listOf(
                Triple(0, "Solid", 0x01), Triple(1, "Comet", 0x1D), Triple(2, "Rainbow", 0x18), Triple(3, "Matrix", 0x1C),
                Triple(4, "Breathe", 0x19), Triple(5, "Heartbeat", 0x19), Triple(6, "Cylon", 0x1C),
                // The reactive ones: Beat Pulse and VU Meter follow sound, Sparkle follows motion.
                Triple(7, "Beat Pulse", 0x3B), Triple(8, "VU Meter", 0x3C), Triple(9, "Sparkle", 0x5C),
            ),
            FakeJacket.EFFECTS.map { Triple(it.id, it.name, it.params.bits) },
        )
        assertEquals(
            listOf(0 to "Colours", 1 to "Rainbow", 2 to "Matrix", 3 to "Cyberpunk", 4 to "Fire", 5 to "Ocean"),
            FakeJacket.PALETTES.map { it.id to it.name },
        )
        assertEquals(listOf("Beat Pulse", "VU Meter"), FakeJacket.EFFECTS.filter { it.params.sound }.map { it.name })
        assertEquals(listOf("Sparkle"), FakeJacket.EFFECTS.filter { it.params.motion }.map { it.name })
    }

    @Test
    fun `a page holds as many entries as fit in a frame, which for v1 is all of them`() = bench {
        send(Command.GetCatalog(CatalogKind.EFFECTS, 0))
        assertEquals(CatalogPage(CatalogKind.EFFECTS, 0, 10, FakeJacket.EFFECTS), received.last())
        assertTrue(Codec.encode(received.last()).size <= Protocol.MAX_FRAME)

        send(Command.GetCatalog(CatalogKind.PALETTES, 2))
        assertEquals(CatalogPage(CatalogKind.PALETTES, 2, 6, FakeJacket.PALETTES.drop(2)), received.last())
    }

    @Test
    fun `asking again with start plus count walks a paged catalogue`() = bench(maxPageEntries = 3) {
        val pages = ArrayList<CatalogPage>()
        var start = 0
        do {
            send(Command.GetCatalog(CatalogKind.EFFECTS, start))
            val page = received.last() as CatalogPage
            pages += page
            start += page.entries.size
        } while (start < page.total)

        assertEquals(listOf(3, 3, 3, 1), pages.map { it.entries.size })
        assertEquals(listOf(0, 3, 6, 9), pages.map { it.start })
        assertTrue(pages.all { it.total == 10 && it.kind == CatalogKind.EFFECTS })
        assertEquals(FakeJacket.EFFECTS, pages.flatMap { it.entries })
    }

    @Test
    fun `a start at or past the total returns an empty page`() = bench {
        for (start in listOf(6, 7, 200)) {
            send(Command.GetCatalog(CatalogKind.PALETTES, start))
            assertEquals(CatalogPage(CatalogKind.PALETTES, start, 6, emptyList()), received.last())
        }
    }

    // ---- state rules -------------------------------------------------------------------------------------------

    @Test
    fun `commands change the state`() = bench {
        apply(Command.SetPower(false), Command.SetBrightness(40))
        assertFalse(state.power)
        assertEquals(40, state.brightness)

        apply(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Color1(Rgb(1, 2, 3))), Command.SetParam(0, Param.Effect(1)))
        assertEquals(Rgb(1, 2, 3), state.segments[0].config.color1)
        assertEquals(1, state.segments[0].config.effect)

        val config = EffectConfig(2, 3, 10, 20, Rgb(0x112233), Rgb(0x445566))
        apply(Command.SetEffect(Protocol.ALL_SEGMENTS, config), Command.SetDevMode(true))
        assertEquals(config, state.segments[0].config)
        assertTrue(state.devMode)
    }

    @Test
    fun `rev increments on every accepted state-changing command, even when the value is unchanged`() = bench {
        val commands = listOf(
            Command.SetPower(true), // already on
            Command.SetBrightness(128), // already 128
            Command.SetBudget(900), // already 900
            Command.SetDevMode(false), // already off
            Command.SetParam(Protocol.ALL_SEGMENTS, Param.Speed(128)), // already 128
            Command.SetSegments(listOf(SegmentGeometry(3, 0, 144, 0))), // already so
            Command.PresetSave(1, "a"),
            Command.PresetLoad(1),
            Command.PresetDelete(1),
        )
        var expected = state.rev
        for (command in commands) {
            apply(command)
            expected++
            assertEquals("after $command", expected, state.rev)
        }
    }

    @Test
    fun `identify, queries and rejected commands leave rev alone and notify no state`() = bench {
        val states = states.size
        send(Command.Identify(3, 0, 10, 5))
        send(Command.GetInfo)
        send(Command.GetPresets)
        send(Command.GetCatalog(CatalogKind.EFFECTS, 0))
        send(Command.SetParam(5, Param.Speed(1))) // no such segment
        send(Command.PresetLoad(3)) // empty slot
        raw("42".hexToBytes())
        delay(300)
        assertEquals(states, this.states.size)
        assertEquals(0, state.rev)
    }

    @Test
    fun `rev wraps at sixteen bits`() = bench {
        val rev = state.rev
        repeat(0x10000) { send(Command.SetPower(true)) }
        delay(200)
        assertEquals("65536 accepted commands later rev is back where it was", rev, state.rev)
    }

    @Test
    fun `the budget is clamped`() = bench {
        apply(Command.SetBudget(5000))
        assertEquals(2000, state.budgetMa)
        apply(Command.SetBudget(10))
        assertEquals(400, state.budgetMa)
        apply(Command.SetBudget(399))
        assertEquals("the jacket idles at about 370 mA, so nothing below 400 is accepted", 400, state.budgetMa)
        apply(Command.SetBudget(401))
        assertEquals(401, state.budgetMa)
    }

    @Test
    fun `live frames raise the live flag, and its two changes each move rev once`() = bench {
        val rev = state.rev
        jacket.sendLive(ByteArray(13)) // too short: dropped silently
        delay(200)
        assertFalse(state.liveInput)

        val frame = Codec.encode(LiveFrame(1, 0, 10, 20, 30, 40, 1200, 0, 0, 0, 0, 0))
        repeat(10) {
            jacket.sendLive(frame + 0x55) // longer than 14 bytes still counts
            delay(100)
        }
        assertTrue(state.liveInput)
        assertEquals(rev + 1, state.rev)
        delay(300)
        assertTrue("400 ms after the last frame", state.liveInput)
        delay(400)
        assertFalse("800 ms after the last frame", state.liveInput)
        assertEquals(rev + 2, state.rev)
        assertTrue(errors.isEmpty())
    }

    // ---- segments and identify ----------------------------------------------------------------------------------

    @Test
    fun `new segments copy segment zero and existing ones keep their effect`() = bench {
        val left = SegmentGeometry(1, 0, 144, 0)
        val right = SegmentGeometry(2, 0, 144, 1)
        apply(Command.SetParam(0, Param.Effect(4)), Command.SetSegments(listOf(left, right)), Command.SetParam(1, Param.Effect(2)))
        assertEquals(listOf(left, right), state.segments.map { it.geometry })
        assertEquals(listOf(4, 2), state.segments.map { it.config.effect })

        apply(Command.SetSegments(listOf(right, left, SegmentGeometry(3, 10, 20, 0))))
        assertEquals(listOf(4, 2, 4), state.segments.map { it.config.effect })

        apply(Command.SetSegments(listOf(left)))
        assertEquals(listOf(4), state.segments.map { it.config.effect })
    }

    @Test
    fun `identify lights a range without touching the state, and zero length or seconds cancels`() = bench {
        send(Command.Identify(2, 30, 10, 5))
        assertEquals(Command.Identify(2, 30, 10, 5), jacket.identified)
        send(Command.Identify(0, 200, 0, 5)) // a cancel is not checked: there is nothing to light
        assertNull(jacket.identified)
        send(Command.Identify(1, 0, 144, 1))
        send(Command.Identify(1, 0, 144, 0))
        assertNull(jacket.identified)
        assertTrue(errors.isEmpty())

        send(Command.Identify(1, 140, 10, 5)) // runs past the strip
        assertEquals(ErrorReply(Opcode.IDENTIFY, ErrorCode.OUT_OF_RANGE), errors.last())
        send(Command.Identify(4, 0, 10, 5)) // no such strip
        assertEquals(2, errors.size)
        assertNull(jacket.identified)
    }

    // ---- presets ------------------------------------------------------------------------------------------------

    @Test
    fun `saving and deleting answer with the updated preset list`() = bench {
        send(Command.GetPresets)
        assertEquals(PresetList(emptyList()), received.last())

        send(Command.PresetSave(2, "Neon"))
        assertEquals(PresetList(listOf(PresetInfo(2, "Neon"))), presetLists.last())
        send(Command.PresetSave(0, "Rave"))
        assertEquals("only used slots, in slot order", PresetList(listOf(PresetInfo(0, "Rave"), PresetInfo(2, "Neon"))), presetLists.last())
        send(Command.PresetSave(2, "Neon II"))
        assertEquals(PresetList(listOf(PresetInfo(0, "Rave"), PresetInfo(2, "Neon II"))), presetLists.last())

        send(Command.PresetDelete(0))
        assertEquals(PresetList(listOf(PresetInfo(2, "Neon II"))), presetLists.last())
        assertEquals("one list per save or delete, plus the one asked for", 5, presetLists.size)
        assertTrue(errors.isEmpty())
    }

    @Test
    fun `a preset stores brightness, segments and effects but not the power`() = bench {
        val layout = listOf(SegmentGeometry(1, 0, 144, 0), SegmentGeometry(2, 0, 72, 1))
        apply(Command.SetBrightness(77), Command.SetSegments(layout), Command.SetParam(1, Param.Effect(3)), Command.PresetSave(2, "Neon"))

        apply(
            Command.SetPower(false),
            Command.SetBrightness(10),
            Command.SetSegments(listOf(SegmentGeometry(3, 0, 144, 0))),
            Command.SetParam(Protocol.ALL_SEGMENTS, Param.Effect(0)),
        )
        apply(Command.PresetLoad(2))
        assertEquals(77, state.brightness)
        assertEquals(layout, state.segments.map { it.geometry })
        assertEquals(listOf(6, 3), state.segments.map { it.config.effect })
        assertFalse("loading does not switch the jacket back on", state.power)
    }

    @Test
    fun `the preset marker follows the document's rules`() = bench {
        apply(Command.PresetSave(2, "Neon"))
        assertEquals("saved: the scene is that slot", 2, state.preset)

        apply(Command.SetPower(false), Command.SetBudget(500), Command.SetDevMode(true))
        assertEquals("power, budget and dev mode do not clear it", 2, state.preset)
        apply(Command.SetBrightness(128), Command.SetParam(0, Param.Speed(128)))
        assertEquals("nor does setting what is already set", 2, state.preset)

        apply(Command.SetBrightness(9))
        assertEquals("a brightness change clears it", Protocol.NO_PRESET, state.preset)
        apply(Command.PresetLoad(2))
        assertEquals("loaded: marked again", 2, state.preset)
        assertEquals(128, state.brightness)

        apply(Command.SetParam(0, Param.Color2(Rgb(1, 1, 1))))
        assertEquals("an effect setting clears it", Protocol.NO_PRESET, state.preset)
        apply(Command.PresetLoad(2))
        apply(Command.SetSegments(listOf(SegmentGeometry(1, 0, 10, 0))))
        assertEquals("a layout change clears it", Protocol.NO_PRESET, state.preset)

        apply(Command.PresetSave(5, "Other"), Command.PresetDelete(2))
        assertEquals("deleting another slot leaves it", 5, state.preset)
        apply(Command.PresetDelete(5))
        assertEquals("deleting the marked slot clears it", Protocol.NO_PRESET, state.preset)
    }

    @Test
    fun `a storage failure is error 5 and changes nothing`() = bench {
        apply(Command.PresetSave(1, "Kept"))
        val lists = presetLists.size
        val rev = state.rev
        jacket.storageFails = true

        send(Command.PresetSave(3, "Lost"))
        assertEquals(ErrorReply(Opcode.PRESET_SAVE, ErrorCode.STORAGE_FAILURE), errors.last())
        send(Command.PresetDelete(1))
        assertEquals(ErrorReply(Opcode.PRESET_DELETE, ErrorCode.STORAGE_FAILURE), errors.last())
        delay(200)
        assertEquals(lists, presetLists.size)
        assertEquals(rev, state.rev)
        assertEquals(1, state.preset)

        jacket.storageFails = false
        send(Command.GetPresets)
        assertEquals(PresetList(listOf(PresetInfo(1, "Kept"))), presetLists.last())
    }

    // ---- validation ---------------------------------------------------------------------------------------------

    @Test
    fun `rejected frames get the code the document gives them`() = bench {
        val before = states.size
        val unknown = ErrorCode.UNKNOWN_OPCODE
        val short = ErrorCode.TOO_SHORT
        val range = ErrorCode.OUT_OF_RANGE
        val missing = ErrorCode.NOT_FOUND
        apply(Command.PresetSave(0, "x"), Command.SetSegments(listOf(SegmentGeometry(1, 0, 144, 0), SegmentGeometry(2, 0, 144, 0))))
        val statesAfterSetup = states.size

        // frame (hex) -> opcode and code of the ERROR it must get
        val cases = listOf(
            "" to (0 to short), // an empty write
            "42" to (0x42 to unknown),
            "07" to (0x07 to unknown),
            "01" to (Opcode.SET_POWER to short),
            "0102" to (Opcode.SET_POWER to range), // on other than 0/1
            "3107" to (Opcode.SET_DEV_MODE to range),
            "02" to (Opcode.SET_BRIGHTNESS to short),
            // SET_EFFECT: too short wins over the bad values it carries
            "03096300" to (Opcode.SET_EFFECT to short),
            "0309010080800000000000ff" to (Opcode.SET_EFFECT to range), // segment 9
            "0302010080800000000000ff" to (Opcode.SET_EFFECT to range), // segment 2 of 2
            "0300630080800000000000ff" to (Opcode.SET_EFFECT to range), // effect 99
            "0300010680800000000000ff" to (Opcode.SET_EFFECT to range), // palette 6
            // SET_PARAM: an unknown id makes the length unknowable, so it is out of range even when cut short
            "0400" to (Opcode.SET_PARAM to short),
            "040009" to (Opcode.SET_PARAM to range),
            "040000" to (Opcode.SET_PARAM to range),
            "040003" to (Opcode.SET_PARAM to short),
            "040005ff00" to (Opcode.SET_PARAM to short),
            "04020380" to (Opcode.SET_PARAM to range), // segment 2 of 2
            "0400010a" to (Opcode.SET_PARAM to range), // effect 10
            "04000206" to (Opcode.SET_PARAM to range), // palette 6
            // SET_SEGMENTS: a bad count is out of range before anything else
            "05" to (Opcode.SET_SEGMENTS to short),
            "0500" to (Opcode.SET_SEGMENTS to range),
            "0509" to (Opcode.SET_SEGMENTS to range),
            "050201009000" to (Opcode.SET_SEGMENTS to short),
            "050100009000" to (Opcode.SET_SEGMENTS to range), // strip_mask 0
            "050104009000" to (Opcode.SET_SEGMENTS to range), // unknown strip bit
            "050101009002" to (Opcode.SET_SEGMENTS to range), // unknown flag bit
            "050101000000" to (Opcode.SET_SEGMENTS to range), // length 0
            "050101019000" to (Opcode.SET_SEGMENTS to range), // 1 + 144 runs past the strip
            "050101009100" to (Opcode.SET_SEGMENTS to range), // length 145
            "060100" to (Opcode.IDENTIFY to short),
            "0600000a05" to (Opcode.IDENTIFY to range), // strip_mask 0, and not a cancel
            // PRESET_SAVE: a name over 16 bytes is out of range whatever follows; otherwise too short comes first
            "10" to (Opcode.PRESET_SAVE to short),
            "1000" to (Opcode.PRESET_SAVE to short),
            "100011" to (Opcode.PRESET_SAVE to range),
            "10000441" to (Opcode.PRESET_SAVE to short),
            "10090441" to (Opcode.PRESET_SAVE to short), // slot 9 too, but the frame is short
            "100800" to (Opcode.PRESET_SAVE to range), // slot 8
            "11" to (Opcode.PRESET_LOAD to short),
            "1108" to (Opcode.PRESET_LOAD to range),
            "1103" to (Opcode.PRESET_LOAD to missing),
            "12" to (Opcode.PRESET_DELETE to short),
            "12ff" to (Opcode.PRESET_DELETE to range),
            "1207" to (Opcode.PRESET_DELETE to missing),
            "2100" to (Opcode.GET_CATALOG to short),
            "2105" to (Opcode.GET_CATALOG to short), // a bad kind, but short comes first
            "210200" to (Opcode.GET_CATALOG to range),
            "3084" to (Opcode.SET_BUDGET to short),
        )
        for ((hex, expected) in cases) {
            val errorsBefore = errors.size
            raw(hex.hexToBytes())
            assertEquals("frame '$hex' must be answered with exactly one error", errorsBefore + 1, errors.size)
            assertEquals("frame '$hex'", ErrorReply(expected.first, expected.second), errors.last())
        }
        delay(300)
        assertEquals("a rejected command changes nothing", statesAfterSetup, states.size)
        assertTrue(before < statesAfterSetup)
    }

    @Test
    fun `trailing bytes are ignored`() = bench {
        raw("0200ffffff".hexToBytes())
        raw("13aabb".hexToBytes())
        delay(200)
        assertEquals(0, state.brightness)
        assertEquals(PresetList(emptyList()), presetLists.last())
        assertTrue(errors.isEmpty())
    }
}
