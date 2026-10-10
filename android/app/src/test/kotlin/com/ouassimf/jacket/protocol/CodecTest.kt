package com.ouassimf.jacket.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

/** What the vectors cannot show: frames that are cut short, padded, unknown or plain noise, and bad input to encode. */
class CodecTest {

    private val vectors = GoldenVector.load()

    @Test
    fun `every strict prefix of every vector is reported as too short`() {
        for (vector in vectors) {
            val bytes = vector.bytes
            for (length in 0 until bytes.size) {
                val e = assertThrows("${vector.name} cut to $length bytes", FrameException::class.java) {
                    vector.decode(bytes.copyOf(length))
                }
                assertEquals("${vector.name} cut to $length bytes", ErrorCode.TOO_SHORT, e.code)
                if (vector.channel != "live") assertEquals(if (length == 0) 0 else bytes[0].toInt() and 0xFF, e.type)
            }
        }
    }

    @Test
    fun `trailing bytes are ignored`() {
        for (vector in vectors) {
            assertEquals(vector.name, vector.decode(), vector.decode(vector.bytes + "deadbeef00ff".hexToBytes()))
        }
    }

    @Test
    fun `an unknown opcode is reported with its opcode`() {
        val e = assertThrows(FrameException::class.java) { Codec.decodeCommand("42010203".hexToBytes()) }
        assertEquals(0x42, e.type)
        assertEquals(ErrorCode.UNKNOWN_OPCODE, e.code)
    }

    @Test
    fun `an unknown notification type is rejected`() {
        val e = assertThrows(FrameException::class.java) { Codec.decodeNotification("0300".hexToBytes()) }
        assertEquals(0x03, e.type)
        assertEquals(ErrorCode.UNKNOWN_OPCODE, e.code)
    }

    @Test
    fun `command values that are not what the document allows are out of range`() {
        for (hex in listOf("0102", "3105", "040009c8", "0400ff00")) {
            val e = assertThrows(hex, FrameException::class.java) { Codec.decodeCommand(hex.hexToBytes()) }
            assertEquals(hex, ErrorCode.OUT_OF_RANGE, e.code)
        }
    }

    @Test
    fun `too short wins over out of range unless the bad value hides the length`() {
        fun code(hex: String) = assertThrows(hex, FrameException::class.java) { Codec.decodeCommand(hex.hexToBytes()) }.code
        // The value that decides the length is bad: out of range, however little follows.
        for (hex in listOf("040009", "0400ff", "0500", "0509", "05ff01", "100011", "1000ff4142")) {
            assertEquals(hex, ErrorCode.OUT_OF_RANGE, code(hex))
        }
        // The length is known and the frame does not reach it: too short, whatever else is wrong with it.
        for (hex in listOf("0400", "040005ff00", "0502010090", "100010414243", "10090441", "01", "31")) {
            assertEquals(hex, ErrorCode.TOO_SHORT, code(hex))
        }
        // Complete frames with values only the jacket can judge decode fine: that is the receiver's check.
        assertEquals(Command.SetSegments(listOf(SegmentGeometry(0, 200, 0, 0xFF))), Codec.decodeCommand("050100c800ff".hexToBytes()))
        assertEquals(Command.PresetSave(200, ""), Codec.decodeCommand("10c800".hexToBytes()))
        assertEquals(Command.GetCatalog(9, 0), Codec.decodeCommand("210900".hexToBytes()))
        assertEquals(Command.PresetSave(0, "sixteen bytes ok"), Codec.decodeCommand(Codec.encode(Command.PresetSave(0, "sixteen bytes ok"))))
    }

    @Test
    fun `names are cut to a byte budget without splitting a character`() {
        assertEquals("Neon", "Neon".truncateUtf8(16))
        assertEquals("sixteen bytes ok", "sixteen bytes ok!".truncateUtf8(16))
        assertEquals("éééééééé", "ééééééééé".truncateUtf8(16)) // two bytes each
        assertEquals("ééééééé", "ééééééééé".truncateUtf8(15)) // not half of the eighth
        assertEquals("⚡⚡⚡⚡⚡", "⚡⚡⚡⚡⚡⚡".truncateUtf8(16)) // three bytes each
        assertEquals("😀😀😀😀", "😀😀😀😀😀".truncateUtf8(16)) // four bytes, and two chars, each
        assertEquals("ab😀", "ab😀😀".truncateUtf8(9))
        assertEquals("", "😀".truncateUtf8(3))
        assertEquals("", "abc".truncateUtf8(0))
        for (text in listOf("Néon ⚡ jacket 😀 très long", "日本語のプリセット名です", "a\uD83D", "")) {
            for (budget in 0..20) {
                val cut = text.truncateUtf8(budget)
                assertTrue("'$text' cut to $budget", cut.utf8Size <= budget && text.startsWith(cut))
            }
        }
        assertEquals(16, "sixteen bytes ok".utf8Size)
        assertEquals(9, "Néon ⚡".utf8Size)
    }

    @Test
    fun `a catalogue page of an unknown kind is rejected`() {
        val e = assertThrows(FrameException::class.java) { Codec.decodeNotification("0202000000".hexToBytes()) }
        assertEquals(ErrorCode.OUT_OF_RANGE, e.code)
    }

    @Test
    fun `state and info from another protocol version are refused`() {
        val state = vectors.first { it.type == "STATE" }.bytes.also { it[1] = 2 }
        val info = vectors.first { it.type == "INFO" }.bytes.also { it[1] = 2 }
        assertEquals(2, assertThrows(UnsupportedProtocolException::class.java) { Codec.decodeNotification(state) }.proto)
        assertEquals(2, assertThrows(UnsupportedProtocolException::class.java) { Codec.decodeNotification(info) }.proto)
    }

    @Test
    fun `a state power byte other than zero means on`() {
        val bytes = vectors.first { it.type == "STATE" }.bytes.also { it[4] = 0x7F }
        assertTrue((Codec.decodeNotification(bytes) as JacketState).power)
    }

    @Test
    fun `noise never escapes as anything but a FrameException`() {
        val random = Random(20261010)
        repeat(20_000) {
            val noise = random.nextBytes(random.nextInt(0, 200))
            for (decode in listOf<(ByteArray) -> Any>(Codec::decodeCommand, Codec::decodeNotification, Codec::decodeLive)) {
                try {
                    decode(noise)
                } catch (e: FrameException) {
                    // The only acceptable way to say no.
                }
            }
        }
    }

    @Test
    fun `whatever decodes from noise encodes back to a prefix of it`() {
        // Strings are excluded: invalid UTF-8 decodes to replacement characters, which do not encode back.
        val random = Random(7)
        var checked = 0
        repeat(20_000) {
            val noise = random.nextBytes(random.nextInt(1, 40))
            noise[0] = FIXED_LAYOUT_OPCODES.random(random).toByte()
            val command = try {
                Codec.decodeCommand(noise)
            } catch (e: FrameException) {
                return@repeat
            }
            val encoded = Codec.encode(command)
            assertEquals(noise.copyOf(encoded.size).toHex(), encoded.toHex())
            checked++
        }
        assertTrue("only $checked frames decoded", checked > 5_000)
    }

    @Test
    fun `the largest state frame fits and round-trips`() {
        val segment = Segment(SegmentGeometry(3, 0, 18, 1), EffectConfig(6, 5, 255, 0, Rgb(0xFFFFFF), Rgb(0x000000)))
        val state = JacketState(1, 65535, true, 255, 2000, 7, 7, List(Protocol.MAX_SEGMENTS) { segment })
        val bytes = Codec.encode(state)
        assertEquals(11 + Protocol.MAX_SEGMENTS * 14, bytes.size)
        assertTrue(bytes.size <= Protocol.MAX_FRAME)
        assertEquals(state, Codec.decodeNotification(bytes))
    }

    @Test
    fun `names are utf-8 with a byte length`() {
        val bytes = Codec.encode(Command.PresetSave(1, "Néon ⚡"))
        assertEquals("Néon ⚡".encodeToByteArray().size, bytes[2].toInt())
        assertEquals(Command.PresetSave(1, "Néon ⚡"), Codec.decodeCommand(bytes))
    }

    @Test
    fun `encoding refuses values that do not fit their field`() {
        val config = EffectConfig(0, 0, 128, 128, Rgb(0), Rgb(0))
        val bad = listOf<() -> Any>(
            { Codec.encode(Command.SetBrightness(256)) },
            { Codec.encode(Command.SetBrightness(-1)) },
            { Codec.encode(Command.SetBudget(70_000)) },
            { Codec.encode(Command.SetEffect(0, config.copy(speed = 300))) },
            { Codec.encode(Command.PresetSave(0, "seventeen bytes!!")) },
            { Codec.encode(CatalogPage(CatalogKind.PALETTES, 0, 1, listOf(EffectInfo(0, ParamFlags(0), "Solid")))) },
            { Codec.encode(CatalogPage(CatalogKind.EFFECTS, 0, 60, List(60) { EffectInfo(it, ParamFlags(0), "Effect") })) },
            { Codec.encode(LiveFrame(0, 0, 0, 0, 0, 0, 0, 0, 0, 200, 0, 0)) },
            { Rgb(256, 0, 0) },
        )
        bad.forEachIndexed { i, block -> assertThrows("case $i", IllegalArgumentException::class.java) { block() } }
    }

    @Test
    fun `rgb packs red first`() {
        val rgb = Rgb(0x00, 0xFF, 0xCC)
        assertEquals(0x00FFCC, rgb.packed)
        assertEquals(listOf(0x00, 0xFF, 0xCC), listOf(rgb.r, rgb.g, rgb.b))
        assertEquals("04ff0500ffcc", Codec.encode(Command.SetParam(0xFF, Param.Color1(rgb))).toHex())
    }

    private companion object {
        /** Opcodes whose payload has no string, so decode → encode must reproduce the input bytes. */
        val FIXED_LAYOUT_OPCODES = listOf(
            Opcode.SET_BRIGHTNESS, Opcode.SET_EFFECT, Opcode.SET_PARAM, Opcode.SET_SEGMENTS, Opcode.IDENTIFY,
            Opcode.PRESET_LOAD, Opcode.PRESET_DELETE, Opcode.GET_PRESETS, Opcode.GET_INFO, Opcode.GET_CATALOG,
            Opcode.SET_BUDGET,
        )
    }
}
