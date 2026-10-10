package com.ouassimf.jacket.data

import com.ouassimf.jacket.link.FakeJacket
import com.ouassimf.jacket.link.JacketLink
import com.ouassimf.jacket.link.LinkFailure
import com.ouassimf.jacket.link.LinkStatus
import com.ouassimf.jacket.link.RecordingLink
import com.ouassimf.jacket.link.RecordingLink.Write
import com.ouassimf.jacket.link.ScriptedLink
import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.EffectConfig
import com.ouassimf.jacket.protocol.EffectInfo
import com.ouassimf.jacket.protocol.ErrorCode
import com.ouassimf.jacket.protocol.ErrorReply
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.Opcode
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.ParamFlags
import com.ouassimf.jacket.protocol.PresetInfo
import com.ouassimf.jacket.protocol.PresetList
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Rgb
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.SegmentGeometry
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
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class JacketRepositoryTest {

    // ---- against the fake jacket: the whole stack with real frames ---------------------------------------------

    /** A repository connected to a fake jacket through a link that records the traffic. */
    private class Bench(private val scope: TestScope, maxPageEntries: Int = Int.MAX_VALUE) {
        val backgroundScope get() = scope.backgroundScope
        fun runCurrent() = scope.runCurrent()

        val jacket = FakeJacket(scope.backgroundScope, maxPageEntries = maxPageEntries)
        val link = RecordingLink(jacket)
        val repository = JacketRepository(scope.backgroundScope, { link }, timeSource = scope.testScheduler.timeSource)
        val state: JacketState get() = repository.state.value!!
        val brightness: Int get() = state.brightness
    }

    private fun ready(block: suspend Bench.() -> Unit) = runTest {
        val bench = Bench(this)
        bench.repository.connect()
        delay(1_000)
        assertEquals(ConnectionStatus.Ready, bench.repository.connection.value)
        bench.link.writes.clear()
        bench.block()
    }

    @Test
    fun `connecting fetches info and both catalogues page by page`() = runTest {
        val bench = Bench(this, maxPageEntries = 4)
        val statuses = ArrayList<ConnectionStatus>()
        val catalogs = ArrayList<Catalog>()
        backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) { bench.repository.connection.collect { statuses += it } }
        backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) { bench.repository.catalog.collect { catalogs += it } }

        bench.repository.connect()
        delay(1_000)

        assertEquals(
            listOf(ConnectionStatus.Disconnected, ConnectionStatus.Connecting, ConnectionStatus.Syncing, ConnectionStatus.Ready),
            statuses,
        )
        assertEquals(FakeJacket.INFO, bench.repository.info.value)
        assertEquals(Catalog(FakeJacket.EFFECTS, FakeJacket.PALETTES, complete = true), bench.repository.catalog.value)
        assertEquals(
            listOf(
                Write(Command.GetInfo, true),
                Write(Command.GetCatalog(CatalogKind.EFFECTS, 0), true),
                Write(Command.GetCatalog(CatalogKind.EFFECTS, 4), true),
                Write(Command.GetCatalog(CatalogKind.EFFECTS, 8), true),
                Write(Command.GetCatalog(CatalogKind.PALETTES, 0), true),
                Write(Command.GetCatalog(CatalogKind.PALETTES, 4), true),
                Write(Command.GetPresets, true),
            ),
            bench.link.writes,
        )
        assertEquals(
            "the catalogue is published as each page lands",
            listOf(0 to 0, 4 to 0, 8 to 0, 10 to 0, 10 to 4, 10 to 6),
            catalogs.map { it.effects.size to it.palettes.size }.distinct(),
        )
        assertEquals(listOf(true), catalogs.filter { it.complete }.map { it.complete })
        assertEquals(1, bench.state.segments.size)
    }

    @Test
    fun `a jacket that sends each catalogue in one page is asked once per catalogue`() = runTest {
        // What the firmware does: as many entries as fit in a frame, which for the v1 catalogue is all of them.
        val bench = Bench(this)
        bench.repository.connect()
        delay(1_000)
        assertEquals(ConnectionStatus.Ready, bench.repository.connection.value)
        assertEquals(Catalog(FakeJacket.EFFECTS, FakeJacket.PALETTES, complete = true), bench.repository.catalog.value)
        assertEquals(
            listOf(Command.GetInfo, Command.GetCatalog(CatalogKind.EFFECTS, 0), Command.GetCatalog(CatalogKind.PALETTES, 0), Command.GetPresets),
            bench.link.writes.map { it.command },
        )
        assertTrue("the jacket boots on, and the app shows what it reports", bench.state.power)
    }

    @Test
    fun `state follows what the jacket reports`() = ready {
        jacket.send(Codec.encode(Command.SetBrightness(9)), withResponse = true) // someone else's change
        jacket.send(Codec.encode(Command.SetParam(0, Param.Effect(1))), withResponse = true)
        jacket.send(Codec.encode(Command.SetPower(false)), withResponse = true)
        delay(200)
        assertEquals(9, brightness)
        assertEquals(1, state.segments[0].config.effect)
        assertFalse(state.power)
    }

    @Test
    fun `a commit shows at once and is written with response`() = ready {
        val rev = state.rev
        repository.commit(Change.Power(false))
        assertFalse("optimistic", state.power)
        assertEquals("not confirmed yet", rev, state.rev)
        delay(200)
        assertEquals(listOf(Write(Command.SetPower(false), true)), link.writes)
        assertFalse(state.power)
        assertEquals(rev + 1, state.rev)
    }

    @Test
    fun `commits are delivered in order`() = ready {
        repository.commit(Change.Power(false))
        repository.commit(Change.Brightness(5))
        repository.commit(Change.SegmentParam(Protocol.ALL_SEGMENTS, Param.Effect(2)))
        repository.commit(Change.Power(true))
        delay(200)
        assertEquals(
            listOf(
                Write(Command.SetPower(false), true),
                Write(Command.SetBrightness(5), true),
                Write(Command.SetParam(Protocol.ALL_SEGMENTS, Param.Effect(2)), true),
                Write(Command.SetPower(true), true),
            ),
            link.writes,
        )
        assertTrue(state.power)
        assertEquals(2, state.segments[0].config.effect)
    }

    @Test
    fun `values produced in one instant collapse to the latest`() = ready {
        repeat(50) { repository.preview(Change.Brightness(it)) }
        runCurrent()
        assertEquals(listOf(Write(Command.SetBrightness(49), false)), link.writes)
    }

    @Test
    fun `previews are spaced by the minimum interval`() = ready {
        repository.preview(Change.Brightness(1))
        runCurrent()
        repository.preview(Change.Brightness(2))
        repository.preview(Change.Brightness(3))
        runCurrent()
        delay(30)
        assertEquals("too early for a second write", listOf(1), link.sent<Command.SetBrightness>().map { it.value })
        repository.preview(Change.Brightness(4))
        delay(10)
        assertEquals(listOf(1, 4), link.sent<Command.SetBrightness>().map { it.value })
        assertTrue(link.writes.none { it.withResponse })
    }

    @Test
    fun `a drag is written about thirty times a second and ends with one reliable write`() = ready {
        repeat(200) {
            repository.preview(Change.Brightness(it))
            delay(5)
        }
        repository.commit(Change.Brightness(200))
        delay(200)

        val previews = link.writes.filter { !it.withResponse }.map { (it.command as Command.SetBrightness).value }
        assertTrue("${previews.size} unacknowledged writes in one second", previews.size in 25..31)
        assertEquals("latest value wins, never an older one", previews.sorted(), previews)
        assertEquals(listOf(Write(Command.SetBrightness(200), true)), link.writes.filter { it.withResponse })
        assertEquals(Write(Command.SetBrightness(200), true), link.writes.last())
        assertEquals(200, brightness)
    }

    @Test
    fun `releasing replaces a preview that was still waiting`() = ready {
        repository.preview(Change.Brightness(1))
        runCurrent()
        repository.preview(Change.Brightness(2)) // waits for the rate limit
        repository.commit(Change.Brightness(3))
        delay(200)
        assertEquals(listOf(Write(Command.SetBrightness(1), false), Write(Command.SetBrightness(3), true)), link.writes)
    }

    @Test
    fun `two controls dragged together are both sent`() = ready {
        val speed = Change.SegmentParam(Protocol.ALL_SEGMENTS, Param.Speed(200))
        val color = Change.SegmentParam(Protocol.ALL_SEGMENTS, Param.Color1(Rgb(1, 2, 3)))
        repository.preview(speed)
        repository.preview(color)
        repository.preview(Change.SegmentParam(0, Param.Speed(7))) // another segment: its own control
        runCurrent()
        assertEquals(
            listOf(
                Command.SetParam(Protocol.ALL_SEGMENTS, Param.Speed(200)),
                Command.SetParam(Protocol.ALL_SEGMENTS, Param.Color1(Rgb(1, 2, 3))),
                Command.SetParam(0, Param.Speed(7)),
            ),
            link.writes.map { it.command },
        )
    }

    @Test
    fun `the jacket's reports never move a control that is being dragged`() = ready {
        var reports = 0
        backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) { jacket.frames.collect { reports++ } }
        val shown = ArrayList<Int>()
        backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) { repository.state.collect { shown += it!!.brightness } }
        shown.clear()

        // Dragging down from 250: every report of the jacket lags behind, i.e. is larger than what is shown.
        for (value in 250 downTo 51) {
            repository.preview(Change.Brightness(value))
            assertEquals(value, brightness)
            delay(5)
            assertEquals(value, brightness)
            if (value == 150) jacket.send(Codec.encode(Command.SetPower(false)), withResponse = true)
        }
        assertTrue("the jacket did report during the drag ($reports)", reports >= 8)
        assertFalse("other fields still come through", state.power)

        repository.commit(Change.Brightness(50))
        repeat(300) {
            assertEquals(50, brightness)
            delay(10)
        }
        assertEquals("the display only ever went down", shown.sortedDescending(), shown)
        assertEquals(50, shown.last())
    }

    @Test
    fun `high connection priority is held while dragging and released a little later`() = ready {
        repository.preview(Change.Brightness(1))
        repository.preview(Change.Brightness(2))
        assertEquals(listOf(true), link.priorities)
        repository.commit(Change.Brightness(3))
        delay(1_000)

        repository.preview(Change.Brightness(4)) // a second drag soon after: still high, nothing to request
        delay(2_500)
        assertEquals(listOf(true), link.priorities)

        repository.commit(Change.Brightness(5))
        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds - 100)
        assertEquals(listOf(true), link.priorities)
        delay(200)
        assertEquals(listOf(true, false), link.priorities)

        repository.commit(Change.Power(false)) // a tap is not a drag
        delay(200)
        assertEquals(listOf(true, false), link.priorities)
    }

    @Test
    fun `live mode holds high priority through drags and reconnects until it lets go`() = ready {
        repository.holdHighPriority(true)
        repository.holdHighPriority(true)
        assertEquals(listOf(true), link.priorities)

        repository.preview(Change.Brightness(1))
        repository.commit(Change.Brightness(2))
        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds + 1_000)
        assertEquals("the end of a drag does not give away what live mode holds", listOf(true), link.priorities)

        jacket.simulateDrop()
        delay(1_000)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals("a new connection starts balanced, so it is asked again", listOf(true, true), link.priorities)

        repository.holdHighPriority(false)
        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds - 100)
        assertEquals(listOf(true, true), link.priorities)
        delay(200)
        assertEquals(listOf(true, true, false), link.priorities)

        repository.preview(Change.Brightness(3)) // and a drag still gets it by itself afterwards
        assertEquals(listOf(true, true, false, true), link.priorities)
    }

    @Test
    fun `a link lost in the middle of a drag is not left asking for high priority`() = ready {
        repository.preview(Change.Brightness(1))
        assertEquals(listOf(true), link.priorities)
        jacket.simulateDrop()
        delay(100)
        // The Bluetooth link remembers what it was last told and asks again on reconnect: it must be told no.
        assertEquals(listOf(true, false), link.priorities)
        delay(60_000)
        assertEquals("and the new connection is left alone", listOf(true, false), link.priorities)
    }

    @Test
    fun `high priority released while the jacket is away is not asked for again when it is back`() = ready {
        repository.holdHighPriority(true)
        repository.disconnect()
        delay(100)
        assertEquals("held across the gap", listOf(true), link.priorities)
        repository.holdHighPriority(false)
        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds + 100)
        assertEquals(listOf(true, false), link.priorities)
        repository.connect()
        delay(2_000)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(listOf(true, false), link.priorities)
    }

    @Test
    fun `a value the jacket clamps snaps to what the jacket says`() = ready {
        repository.commit(Change.Budget(5000))
        assertEquals(5000, state.budgetMa)
        delay(JacketRepository.CONFIRM_TIMEOUT.inWholeMilliseconds + 100)
        assertEquals(2000, state.budgetMa)
    }

    @Test
    fun `a dropped link syncs again and keeps the catalogue meanwhile`() = ready {
        repository.preview(Change.Brightness(1))
        runCurrent()
        jacket.simulateDrop()
        runCurrent()
        assertEquals(ConnectionStatus.Connecting, repository.connection.value)
        assertTrue(repository.catalog.value.complete)
        repository.commit(Change.Brightness(77)) // nowhere to send it: ignored, not queued for later
        assertEquals(1, brightness)

        delay(1_000)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(1, link.sent<Command.GetInfo>().size)
        assertEquals(1, brightness)
        assertTrue(link.sent<Command.SetBrightness>().none { it.value == 77 })
    }

    @Test
    fun `demo mode swaps the link and forgets the old jacket`() = runTest {
        val real = ScriptedLink()
        val demo = FakeJacket(backgroundScope)
        val asked = ArrayList<Boolean>()
        val repository = JacketRepository(
            backgroundScope,
            { isDemo -> asked += isDemo; if (isDemo) demo else real },
            timeSource = testScheduler.timeSource,
        )
        repository.connect()
        real.status.value = LinkStatus.Connected
        real.notify(sampleState(brightness = 42))
        delay(100)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(42, repository.state.value?.brightness)

        repository.setDemoMode(true)
        assertEquals(listOf(false, true), asked)
        assertTrue(real.closed)
        assertTrue(repository.demoMode.value)
        assertNull(repository.state.value)
        assertNull(repository.info.value)
        assertEquals(Catalog(), repository.catalog.value)
        assertEquals(ConnectionStatus.Disconnected, repository.connection.value)

        repository.connect()
        delay(1_000)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(FakeJacket.EFFECTS.size, repository.catalog.value.effects.size)
        repository.commit(Change.Brightness(3))
        delay(200)
        assertEquals(3, repository.state.value?.brightness)
        assertTrue("the old link hears nothing more", real.writes.none { it.command is Command.SetBrightness })

        repository.setDemoMode(true) // no change: must not rebuild anything
        assertEquals(listOf(false, true), asked)
    }

    // ---- against a scripted link: jackets that misbehave --------------------------------------------------------

    private class Script(scope: TestScope) {
        val link = ScriptedLink()
        val repository = JacketRepository(scope.backgroundScope, { link as JacketLink }, timeSource = scope.testScheduler.timeSource)
        val brightness: Int? get() = repository.state.value?.brightness

        suspend fun connectWith(state: JacketState) {
            repository.connect()
            link.status.value = LinkStatus.Connected
            link.notify(state)
            delay(50)
        }
    }

    private fun scripted(block: suspend Script.() -> Unit) = runTest { Script(this).block() }

    @Test
    fun `a stale report after release does not pull the control back`() = scripted {
        connectWith(sampleState(brightness = 100, rev = 1))
        assertEquals(ConnectionStatus.Ready, repository.connection.value)

        repository.preview(Change.Brightness(110))
        repository.commit(Change.Brightness(120))
        link.notify(sampleState(brightness = 110, rev = 2)) // sent by the jacket before it saw 120
        delay(50)
        assertEquals(120, brightness)

        link.notify(sampleState(brightness = 120, rev = 3)) // the confirmation
        delay(50)
        assertEquals(120, brightness)

        link.notify(sampleState(brightness = 30, rev = 4)) // a later change from elsewhere is believed again
        delay(50)
        assertEquals(30, brightness)
    }

    @Test
    fun `a commit the jacket never confirms is given up after the timeout`() = scripted {
        connectWith(sampleState(brightness = 100))
        repository.commit(Change.Brightness(120))
        delay(JacketRepository.CONFIRM_TIMEOUT.inWholeMilliseconds - 100)
        assertEquals(120, brightness)
        delay(200)
        assertEquals(100, brightness)
    }

    @Test
    fun `a newer commit is not expired by the timer of an older one`() = scripted {
        connectWith(sampleState(brightness = 100))
        repository.commit(Change.Brightness(120))
        delay(1_000)
        repository.commit(Change.Brightness(130))
        delay(1_000) // the first timer has fired by now
        assertEquals(130, brightness)
        delay(1_000)
        assertEquals(100, brightness)
    }

    @Test
    fun `changes are ignored until a state is known`() = scripted {
        repository.commit(Change.Power(false))
        repository.preview(Change.Brightness(1))
        repository.connect()
        link.status.value = LinkStatus.Connected
        repository.commit(Change.Power(false)) // connected, but no STATE yet: nothing to apply the change to
        delay(50)
        assertNull(repository.state.value)
        assertTrue(link.writes.none { it.command is Command.SetPower || it.command is Command.SetBrightness })
    }

    @Test
    fun `losing the link drops pending work and unconfirmed values`() = scripted {
        connectWith(sampleState(brightness = 100))
        repository.preview(Change.Brightness(200))
        assertEquals(200, brightness)
        link.status.value = LinkStatus.Connecting
        delay(50)
        assertEquals(ConnectionStatus.Connecting, repository.connection.value)
        assertEquals(100, brightness)
    }

    @Test
    fun `a jacket speaking another protocol version is refused`() = scripted {
        repository.connect()
        link.status.value = LinkStatus.Connected
        link.raw(Codec.encode(sampleState()).also { it[1] = 2 })
        delay(50)
        assertEquals(ConnectionStatus.Failed(ConnectionFailure.UnsupportedProtocol), repository.connection.value)
        assertEquals(1, link.disconnects)
        assertNull(repository.state.value)

        repository.connect() // retry clears the failure
        link.status.value = LinkStatus.Connected
        link.notify(sampleState())
        delay(50)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
    }

    @Test
    fun `a jacket that never answers is given up on after three attempts`() = scripted {
        link.onCommand = {}
        connectWith(sampleState())
        assertEquals(ConnectionStatus.Syncing, repository.connection.value)
        repository.commit(Change.Brightness(1)) // usable while syncing
        delay(5_900)
        assertEquals(ConnectionStatus.Syncing, repository.connection.value)
        delay(200)
        assertEquals(ConnectionStatus.Failed(ConnectionFailure.NoReply), repository.connection.value)
        assertEquals(3, link.writes.count { it.command == Command.GetInfo })
        assertEquals(1, link.writes.count { it.command == Command.SetBrightness(1) })
        assertEquals(1, link.disconnects)
    }

    @Test
    fun `a lost reply is asked for again`() = scripted {
        var lost = false
        link.onCommand = { command ->
            if (command is Command.GetCatalog && command.start == 0 && command.kind == CatalogKind.PALETTES && !lost) {
                lost = true
            } else {
                link.answerQueries(command)
            }
        }
        connectWith(sampleState())
        assertEquals(ConnectionStatus.Syncing, repository.connection.value)
        delay(2_100)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(FakeJacket.PALETTES, repository.catalog.value.palettes)
    }

    @Test
    fun `an error reply to a query fails the sync without waiting`() = scripted {
        link.onCommand = { link.notify(ErrorReply(Opcode.GET_INFO, ErrorCode.UNKNOWN_OPCODE)) }
        connectWith(sampleState())
        assertEquals(ConnectionStatus.Failed(ConnectionFailure.NoReply), repository.connection.value)
        assertNull("a failed query of the app is not a refused user command", repository.error.value)
    }

    @Test
    fun `pages are matched to the request and odd ones are ignored`() = scripted {
        val effects = List(5) { EffectInfo(it, ParamFlags(it), "Effect $it") }
        link.onCommand = { command ->
            when (command) {
                Command.GetInfo -> link.notify(FakeJacket.INFO)
                is Command.GetCatalog -> if (command.kind == CatalogKind.EFFECTS) {
                    link.notify(CatalogPage(CatalogKind.EFFECTS, 99, 5, effects)) // not what was asked: ignored
                    link.notify(CatalogPage(CatalogKind.PALETTES, command.start, 1, FakeJacket.PALETTES.take(1)))
                    link.notify(CatalogPage(CatalogKind.EFFECTS, command.start, 5, effects.drop(command.start).take(2)))
                } else {
                    link.notify(CatalogPage(CatalogKind.PALETTES, command.start, 9, emptyList())) // lies about total
                }
                else -> Unit
            }
        }
        connectWith(sampleState())
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals(Catalog(effects, emptyList(), complete = true), repository.catalog.value)
        assertEquals(listOf(0, 2, 4, 0), link.writes.mapNotNull { (it.command as? Command.GetCatalog)?.start })
    }

    @Test
    fun `garbage and unknown notifications are ignored`() = scripted {
        connectWith(sampleState(brightness = 100))
        link.raw(byteArrayOf())
        link.raw(byteArrayOf(0x55, 1, 2, 3))
        link.raw(Codec.encode(sampleState(brightness = 1)).copyOf(12))
        link.notify(ErrorReply(Opcode.SET_POWER, ErrorCode.OUT_OF_RANGE))
        delay(50)
        assertEquals(100, brightness)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
    }

    @Test
    fun `link failures are reported as they are`() = scripted {
        val expected = mapOf(
            LinkFailure.BluetoothOff to ConnectionFailure.BluetoothOff,
            LinkFailure.PermissionMissing to ConnectionFailure.PermissionMissing,
            LinkFailure.MtuTooLow to ConnectionFailure.MtuTooLow,
        )
        assertEquals(LinkFailure.entries.toSet(), expected.keys)
        for ((linkFailure, failure) in expected) {
            link.status.value = LinkStatus.Failed(linkFailure)
            delay(10)
            assertEquals(ConnectionStatus.Failed(failure), repository.connection.value)
        }
        link.status.value = LinkStatus.Scanning
        delay(10)
        assertEquals(ConnectionStatus.Scanning, repository.connection.value)
    }

    @Test
    fun `a reconnect keeps the old catalogue up until the new one is whole`() = scripted {
        connectWith(sampleState())
        val old = repository.catalog.value
        assertTrue(old.complete)

        val pending = ArrayList<Command>()
        link.onCommand = { pending += it }
        link.status.value = LinkStatus.Connecting
        delay(10)
        link.status.value = LinkStatus.Connected
        delay(10)
        assertEquals(ConnectionStatus.Syncing, repository.connection.value)
        assertEquals(listOf<Command>(Command.GetInfo), pending)

        val renamed = listOf(EffectInfo(0, ParamFlags(1), "Only one now"))
        link.notify(FakeJacket.INFO)
        delay(10)
        link.notify(CatalogPage(CatalogKind.EFFECTS, 0, 1, renamed))
        delay(10)
        assertEquals("still the old one while the new one is incomplete", old, repository.catalog.value)

        link.notify(CatalogPage(CatalogKind.PALETTES, 0, 1, FakeJacket.PALETTES.take(1)))
        delay(10)
        assertEquals(Catalog(renamed, FakeJacket.PALETTES.take(1), complete = true), repository.catalog.value)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
    }

    // ---- presets --------------------------------------------------------------------------------------------------

    @Test
    fun `the preset list is asked for once at connect`() = ready {
        assertEquals(emptyList<PresetInfo>(), repository.presets.value)
        assertEquals(Protocol.NO_PRESET, state.preset)
    }

    @Test
    fun `saving updates the list from the jacket's own notification and marks the slot active`() = ready {
        repository.savePreset(2, "Neon")
        assertEquals("nothing is assumed before the jacket answers", emptyList<PresetInfo>(), repository.presets.value)
        delay(200)

        assertEquals(listOf(Write(Command.PresetSave(2, "Neon"), true)), link.writes)
        assertEquals(listOf(PresetInfo(2, "Neon")), repository.presets.value)
        assertEquals(2, state.preset)
        assertNull(repository.error.value)

        repository.savePreset(0, "Rave")
        repository.savePreset(2, "Neon II")
        delay(200)
        assertEquals(listOf(PresetInfo(0, "Rave"), PresetInfo(2, "Neon II")), repository.presets.value)
        assertEquals(2, state.preset)
        assertTrue("the list is never polled", link.sent<Command.GetPresets>().isEmpty())
    }

    @Test
    fun `the active mark goes when the scene changes and comes back on load`() = ready {
        repository.savePreset(4, "Calm")
        delay(200)
        assertEquals(4, state.preset)

        repository.commit(Change.Power(false))
        delay(200)
        assertEquals("power is not part of a preset", 4, state.preset)

        repository.commit(Change.Brightness(20))
        delay(200)
        assertEquals(Protocol.NO_PRESET, state.preset)

        repository.loadPreset(4)
        delay(200)
        assertEquals(4, state.preset)
        assertEquals(128, brightness)
        assertFalse("loading does not touch the power", state.power)
        assertEquals(Write(Command.PresetLoad(4), true), link.writes.last())
    }

    @Test
    fun `deleting updates the list and clears the mark of that slot only`() = ready {
        repository.savePreset(1, "One")
        repository.savePreset(6, "Six")
        delay(200)
        assertEquals(6, state.preset)

        repository.deletePreset(1)
        delay(200)
        assertEquals(listOf(PresetInfo(6, "Six")), repository.presets.value)
        assertEquals(6, state.preset)

        repository.deletePreset(6)
        delay(200)
        assertEquals(emptyList<PresetInfo>(), repository.presets.value)
        assertEquals(Protocol.NO_PRESET, state.preset)
        assertNull(repository.error.value)
    }

    @Test
    fun `loading or deleting an empty slot surfaces error 4 until dismissed`() = ready {
        val before = state
        repository.loadPreset(3)
        delay(200)
        assertEquals(ErrorReply(Opcode.PRESET_LOAD, ErrorCode.NOT_FOUND), repository.error.value)
        assertEquals(before, state)

        repository.dismissError()
        assertNull(repository.error.value)

        repository.deletePreset(3)
        delay(200)
        assertEquals(ErrorReply(Opcode.PRESET_DELETE, ErrorCode.NOT_FOUND), repository.error.value)
        assertEquals(emptyList<PresetInfo>(), repository.presets.value)
    }

    @Test
    fun `a storage failure surfaces error 5 and leaves the list and the mark alone`() = ready {
        repository.savePreset(1, "Kept")
        delay(200)
        jacket.storageFails = true

        repository.savePreset(5, "Lost")
        delay(200)
        assertEquals(ErrorReply(Opcode.PRESET_SAVE, ErrorCode.STORAGE_FAILURE), repository.error.value)
        assertEquals(listOf(PresetInfo(1, "Kept")), repository.presets.value)
        assertEquals(1, state.preset)

        repository.deletePreset(1)
        delay(200)
        assertEquals(ErrorReply(Opcode.PRESET_DELETE, ErrorCode.STORAGE_FAILURE), repository.error.value)
        assertEquals(listOf(PresetInfo(1, "Kept")), repository.presets.value)
    }

    @Test
    fun `a name is limited to sixteen bytes, not sixteen characters`() = ready {
        assertThrows(IllegalArgumentException::class.java) { repository.savePreset(0, "ééééééééé") } // 9 characters, 18 bytes
        assertThrows(IllegalArgumentException::class.java) { repository.savePreset(0, "seventeen bytes!!") }
        delay(200)
        assertTrue(link.writes.isEmpty())

        repository.savePreset(0, "éééééééé") // 8 characters, 16 bytes
        delay(200)
        assertEquals(listOf(PresetInfo(0, "éééééééé")), repository.presets.value)
    }

    @Test
    fun `a jacket that does not list its presets is still usable, and can be asked again`() = scripted {
        link.onCommand = { if (it != Command.GetPresets) link.answerQueries(it) }
        connectWith(sampleState())
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        delay(7_000) // the three attempts have run out
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertNull(repository.presets.value)
        assertEquals(3, link.sent<Command.GetPresets>().size)

        link.onCommand = { if (it == Command.GetPresets) link.notify(PresetList(listOf(PresetInfo(7, "Late")))) }
        repository.refreshPresets()
        delay(50)
        assertEquals(listOf(PresetInfo(7, "Late")), repository.presets.value)
    }

    @Test
    fun `a preset list the jacket volunteers is taken as it comes`() = scripted {
        connectWith(sampleState())
        link.notify(PresetList(listOf(PresetInfo(0, "A"), PresetInfo(3, "B"))))
        delay(50)
        assertEquals(listOf(PresetInfo(0, "A"), PresetInfo(3, "B")), repository.presets.value)
    }

    @Test
    fun `the preset list may come before or after the state of the same command`() = scripted {
        // The real jacket usually sends PRESETS first; the document leaves the order open.
        connectWith(sampleState())
        var presetsFirst = true
        link.onCommand = { command ->
            if (command is Command.PresetSave) {
                val list = PresetList(listOf(PresetInfo(command.slot, command.name)))
                val state = sampleState(rev = 2 + command.slot).copy(preset = command.slot)
                if (presetsFirst) {
                    link.notify(list)
                    link.notify(state)
                } else {
                    link.notify(state)
                    link.notify(list)
                }
            }
        }
        repository.savePreset(2, "Night")
        delay(50)
        assertEquals(listOf(PresetInfo(2, "Night")), repository.presets.value)
        assertEquals(2, repository.state.value!!.preset)

        presetsFirst = false
        repository.savePreset(5, "Rave")
        delay(50)
        assertEquals(listOf(PresetInfo(5, "Rave")), repository.presets.value)
        assertEquals(5, repository.state.value!!.preset)
        assertNull(repository.error.value)
    }

    @Test
    fun `a jacket that was reset and counts rev from zero again is believed`() = scripted {
        connectWith(sampleState(brightness = 100, rev = 40_000))
        assertEquals(100, brightness)
        link.status.value = LinkStatus.Connecting // it rebooted
        delay(50)
        link.status.value = LinkStatus.Connected
        link.notify(sampleState(brightness = 7, rev = 0))
        delay(50)
        assertEquals(ConnectionStatus.Ready, repository.connection.value)
        assertEquals("a lower rev is not an older state", 7, brightness)
        assertEquals(0, repository.state.value!!.rev)
        link.notify(sampleState(brightness = 9, rev = 1))
        delay(50)
        assertEquals(9, brightness)
    }

    @Test
    fun `the state that acknowledges a command may take its time`() = scripted {
        // Measured on the board: the STATE after a write comes 60 ms later at the median, 120 ms at worst, and
        // whatever was read or notified in between is older than the command.
        connectWith(sampleState(brightness = 100, rev = 1))
        repository.commit(Change.Brightness(200))
        delay(20)
        link.notify(sampleState(brightness = 100, rev = 1)) // an echo of the state before the write
        delay(100)
        assertEquals("still showing what was asked for", 200, brightness)
        link.notify(sampleState(brightness = 200, rev = 2))
        delay(50)
        assertEquals(200, brightness)
        delay(JacketRepository.CONFIRM_TIMEOUT.inWholeMilliseconds)
        assertEquals("confirmed, so the timeout changes nothing", 200, brightness)
    }

    @Test
    fun `a refused change stops being shown as soon as the error arrives`() = scripted {
        connectWith(sampleState(brightness = 100))
        repository.commit(Change.Brightness(120))
        repository.commit(Change.Power(false))
        assertEquals(120, brightness)

        link.notify(ErrorReply(Opcode.SET_BRIGHTNESS, ErrorCode.OUT_OF_RANGE))
        delay(50)
        assertEquals("back to what the jacket says, without waiting for the timeout", 100, brightness)
        assertEquals(ErrorReply(Opcode.SET_BRIGHTNESS, ErrorCode.OUT_OF_RANGE), repository.error.value)
        assertFalse("the change that was not refused is still pending", repository.state.value!!.power)
    }

    @Test
    fun `demo mode forgets presets and errors of the other jacket`() = scripted {
        connectWith(sampleState())
        link.notify(PresetList(listOf(PresetInfo(0, "A"))))
        link.notify(ErrorReply(Opcode.PRESET_LOAD, ErrorCode.NOT_FOUND))
        delay(50)
        assertEquals(listOf(PresetInfo(0, "A")), repository.presets.value)

        repository.setDemoMode(true)
        assertNull(repository.presets.value)
        assertNull(repository.error.value)
    }

    // ---- zones ------------------------------------------------------------------------------------------------------

    @Test
    fun `a layout is shown at once and sent as one command`() = ready {
        val cylon = state.segments[0].config
        val layout = listOf(Segment(SegmentGeometry(1, 0, 144, 0), cylon), Segment(SegmentGeometry(2, 0, 144, 1), cylon))
        val rev = state.rev

        repository.applyLayout(layout)
        assertEquals("optimistic", layout, state.segments)
        assertEquals(rev, state.rev)
        delay(200)

        assertEquals(listOf(Write(Command.SetSegments(layout.map { it.geometry }), true)), link.writes)
        assertEquals(layout, state.segments)
        assertEquals(rev + 1, state.rev)
    }

    @Test
    fun `zones keep their effects when one in the middle is removed`() = ready {
        val a = Segment(SegmentGeometry(1, 0, 48, 0), state.segments[0].config.copy(effect = 0))
        val b = Segment(SegmentGeometry(1, 48, 48, 0), state.segments[0].config.copy(effect = 1))
        val c = Segment(SegmentGeometry(1, 96, 48, 0), state.segments[0].config.copy(effect = 2, speed = 200))
        repository.applyLayout(listOf(a, b, c))
        delay(200)
        assertEquals(listOf(a, b, c), state.segments)
        link.writes.clear()

        repository.applyLayout(listOf(a, c)) // b is gone: c moves to index 1, where the jacket would leave b's effect
        delay(200)
        assertEquals(
            listOf(Write(Command.SetSegments(listOf(a.geometry, c.geometry)), true), Write(Command.SetEffect(1, c.config), true)),
            link.writes,
        )
        assertEquals(listOf(a, c), state.segments)
    }

    @Test
    fun `layout commands only repair what set segments gets wrong`() {
        fun segment(start: Int, effect: Int) = Segment(SegmentGeometry(3, start, 10, 0), EffectConfig(effect, 0, 128, 128, Rgb(1), Rgb(2)))
        val old = listOf(segment(0, 4), segment(10, 5), segment(20, 6))
        fun effects(new: List<Segment>) = layoutCommands(old, new).drop(1).map { it as Command.SetEffect }.map { it.segment to it.config.effect }

        val resized = old.map { it.copy(geometry = it.geometry.copy(length = 5)) }
        assertEquals(listOf<Command>(Command.SetSegments(resized.map { it.geometry })), layoutCommands(old, resized))
        assertEquals("a new zone that takes segment 0's effect needs nothing", emptyList<Pair<Int, Int>>(), effects(old + segment(30, 4)))
        assertEquals("a new zone with its own effect does", listOf(3 to 9), effects(old + segment(30, 9)))
        assertEquals("removing the first shifts the other two", listOf(0 to 5, 1 to 6), effects(old.drop(1)))
        assertEquals("removing the last shifts nothing", emptyList<Pair<Int, Int>>(), effects(old.dropLast(1)))
        assertEquals("swapping two", listOf(0 to 5, 1 to 4), effects(listOf(old[1], old[0], old[2])))
    }

    @Test
    fun `a layout the jacket refuses snaps back and surfaces the error`() = scripted {
        connectWith(sampleState(segments = 2))
        val before = repository.state.value!!.segments
        link.onCommand = { if (it is Command.SetSegments) link.notify(ErrorReply(Opcode.SET_SEGMENTS, ErrorCode.OUT_OF_RANGE)) }

        repository.applyLayout(before.take(1))
        assertEquals(1, repository.state.value!!.segments.size)
        delay(50)
        assertEquals(before, repository.state.value!!.segments)
        assertEquals(ErrorReply(Opcode.SET_SEGMENTS, ErrorCode.OUT_OF_RANGE), repository.error.value)
    }

    @Test
    fun `one zone can be edited without touching the others`() = ready {
        val cylon = state.segments[0].config
        repository.applyLayout(listOf(Segment(SegmentGeometry(1, 0, 144, 0), cylon), Segment(SegmentGeometry(2, 0, 144, 0), cylon)))
        delay(200)
        link.writes.clear()

        repository.preview(Change.SegmentParam(1, Param.Speed(10)))
        repository.commit(Change.SegmentParam(1, Param.Speed(20)))
        repository.commit(Change.SegmentParam(1, Param.Effect(2)))
        assertEquals("shown at once, on that zone only", listOf(6, 2), state.segments.map { it.config.effect })
        delay(200)

        assertEquals(listOf(128, 20), state.segments.map { it.config.speed })
        assertEquals(listOf(6, 2), state.segments.map { it.config.effect })
        assertTrue(link.sent<Command.SetParam>().all { it.segment == 1 })

        repository.commit(Change.SegmentParam(Protocol.ALL_SEGMENTS, Param.Effect(0)))
        delay(200)
        assertEquals(listOf(0, 0), state.segments.map { it.config.effect })
    }

    @Test
    fun `which zone home edits follows the selection and falls back to all`() {
        val all = SegmentTarget(Protocol.ALL_SEGMENTS, 0)
        assertEquals("nothing picked", all, SegmentTarget.of(null, 3))
        assertEquals(SegmentTarget(0, 0), SegmentTarget.of(0, 3))
        assertEquals(SegmentTarget(2, 2), SegmentTarget.of(2, 3))
        assertEquals("the zone is gone", all, SegmentTarget.of(3, 3))
        assertEquals("one zone is all zones", all, SegmentTarget.of(0, 1))
        assertEquals("no state yet", all, SegmentTarget.of(1, 0))
        assertEquals(all, SegmentTarget.of(-1, 3))
    }

    @Test
    fun `identify follows a drag unacknowledged and ends it with one acknowledged command`() = ready {
        val rev = state.rev
        repeat(100) {
            repository.identify(stripMask = 1, start = it, length = 10, seconds = 3, dragging = true)
            delay(5)
        }
        assertEquals(listOf(true), link.priorities)
        repository.identify(stripMask = 1, start = 100, length = 10, seconds = 3)
        delay(200)

        val sent = link.writes.map { it.command as Command.Identify }
        assertTrue("${sent.size} writes for half a second of dragging", sent.size in 12..18)
        assertEquals(sent.map { it.start }.sorted(), sent.map { it.start })
        assertTrue(link.writes.dropLast(1).none { it.withResponse })
        assertEquals(Write(Command.Identify(1, 100, 10, 3), true), link.writes.last())
        assertEquals(Command.Identify(1, 100, 10, 3), jacket.identified)
        assertEquals("identify is not part of the state", rev, state.rev)

        delay(JacketRepository.PRIORITY_LINGER.inWholeMilliseconds)
        assertEquals(listOf(true, false), link.priorities)

        repository.identify(1, 0, 0, 0) // cancel
        delay(50)
        assertNull(jacket.identified)
    }

    private companion object {
        fun sampleState(brightness: Int = 128, rev: Int = 1, segments: Int = 1) = JacketState(
            proto = Protocol.VERSION,
            rev = rev,
            power = true,
            brightness = brightness,
            budgetMa = 900,
            flags = 0,
            preset = Protocol.NO_PRESET,
            segments = List(segments) {
                Segment(SegmentGeometry(3, 10 * it, 10, 0), EffectConfig(1 + it, 0, 128, 128, Rgb(0x00FFCC), Rgb(0xFF00FF)))
            },
        )
    }
}
