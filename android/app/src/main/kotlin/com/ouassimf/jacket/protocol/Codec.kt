package com.ouassimf.jacket.protocol

/**
 * A frame that cannot be decoded. [type] is the frame's first byte (0 when the frame is empty) and [code] one of
 * [ErrorCode], so the receiving side can answer `ERROR(type, code)` directly.
 */
open class FrameException(val type: Int, val code: Int, message: String) : IllegalArgumentException(message)

/** A STATE or INFO frame from a jacket speaking another protocol version; its layout cannot be trusted. */
class UnsupportedProtocolException(type: Int, val proto: Int) :
    FrameException(type, ErrorCode.OUT_OF_RANGE, "protocol $proto, this app speaks ${Protocol.VERSION}")

/**
 * Encodes and decodes every frame of the protocol, in both directions: the app encodes commands and decodes
 * notifications, the fake jacket (and the golden-vector tests) do the opposite.
 *
 * Decoders follow the document: trailing bytes are ignored, frames that are too short throw [FrameException].
 * Encoders throw [IllegalArgumentException] for values that do not fit their field.
 *
 * [decodeCommand] applies the document's precedence: "too short" (code 2) wins over "out of range" (code 3),
 * except where a bad value leaves the frame's length unknowable (`SET_PARAM` id, segment count, name length).
 * It checks what a frame says about itself; what depends on the jacket (does this effect exist, is this segment
 * there, does the range fit the strip) is for the receiver to check afterwards.
 */
object Codec {

    // ---- command characteristic -------------------------------------------------------------------------------

    fun encode(command: Command): ByteArray = FrameWriter().apply {
        u8(command.opcode)
        when (command) {
            is Command.SetPower -> bool(command.on)
            is Command.SetBrightness -> u8(command.value)
            is Command.SetEffect -> {
                u8(command.segment)
                config(command.config)
            }
            is Command.SetParam -> {
                u8(command.segment)
                u8(command.param.id)
                when (val p = command.param) {
                    is Param.Effect -> u8(p.value)
                    is Param.Palette -> u8(p.value)
                    is Param.Speed -> u8(p.value)
                    is Param.Intensity -> u8(p.value)
                    is Param.Color1 -> rgb(p.value)
                    is Param.Color2 -> rgb(p.value)
                }
            }
            is Command.SetSegments -> {
                u8(command.segments.size)
                command.segments.forEach(::geometry)
            }
            is Command.Identify -> {
                u8(command.stripMask)
                u8(command.start)
                u8(command.length)
                u8(command.seconds)
            }
            is Command.PresetSave -> {
                u8(command.slot)
                require(command.name.encodeToByteArray().size <= Protocol.MAX_PRESET_NAME_BYTES) {
                    "preset name longer than ${Protocol.MAX_PRESET_NAME_BYTES} bytes"
                }
                str(command.name)
            }
            is Command.PresetLoad -> u8(command.slot)
            is Command.PresetDelete -> u8(command.slot)
            Command.GetPresets, Command.GetInfo -> Unit
            is Command.GetCatalog -> {
                u8(command.kind)
                u8(command.start)
            }
            is Command.SetBudget -> u16(command.milliamps)
            is Command.SetDevMode -> bool(command.on)
            is Command.BootOtherSlot -> u8(command.confirm)
        }
    }.toByteArray()

    fun decodeCommand(frame: ByteArray): Command {
        val r = FrameReader(frame)
        return when (val opcode = r.type) {
            Opcode.SET_POWER -> Command.SetPower(r.bool())
            Opcode.SET_BRIGHTNESS -> Command.SetBrightness(r.u8())
            Opcode.SET_EFFECT -> Command.SetEffect(r.u8(), r.config())
            Opcode.SET_PARAM -> {
                val segment = r.u8()
                val param = when (val id = r.u8()) {
                    ParamId.EFFECT -> Param.Effect(r.u8())
                    ParamId.PALETTE -> Param.Palette(r.u8())
                    ParamId.SPEED -> Param.Speed(r.u8())
                    ParamId.INTENSITY -> Param.Intensity(r.u8())
                    ParamId.COLOR1 -> Param.Color1(r.rgb())
                    ParamId.COLOR2 -> Param.Color2(r.rgb())
                    else -> throw FrameException(opcode, ErrorCode.OUT_OF_RANGE, "unknown param $id")
                }
                Command.SetParam(segment, param)
            }
            Opcode.SET_SEGMENTS -> {
                val count = r.u8()
                if (count !in 1..Protocol.MAX_SEGMENTS) throw FrameException(opcode, ErrorCode.OUT_OF_RANGE, "$count segments")
                Command.SetSegments(List(count) { r.geometry() })
            }
            Opcode.IDENTIFY -> Command.Identify(r.u8(), r.u8(), r.u8(), r.u8())
            Opcode.PRESET_SAVE -> Command.PresetSave(r.u8(), r.str(maxBytes = Protocol.MAX_PRESET_NAME_BYTES))
            Opcode.PRESET_LOAD -> Command.PresetLoad(r.u8())
            Opcode.PRESET_DELETE -> Command.PresetDelete(r.u8())
            Opcode.GET_PRESETS -> Command.GetPresets
            Opcode.GET_INFO -> Command.GetInfo
            Opcode.GET_CATALOG -> Command.GetCatalog(r.u8(), r.u8())
            Opcode.SET_BUDGET -> Command.SetBudget(r.u16())
            Opcode.SET_DEV_MODE -> Command.SetDevMode(r.bool())
            Opcode.BOOT_OTHER_SLOT -> Command.BootOtherSlot(r.u8())
            else -> throw FrameException(opcode, ErrorCode.UNKNOWN_OPCODE, "unknown opcode $opcode")
        }
    }

    // ---- state characteristic ---------------------------------------------------------------------------------

    fun encode(notification: Notification): ByteArray = FrameWriter().apply {
        when (notification) {
            is JacketState -> {
                u8(NotificationType.STATE)
                u8(notification.proto)
                u16(notification.rev)
                bool(notification.power)
                u8(notification.brightness)
                u16(notification.budgetMa)
                u8(notification.flags)
                u8(notification.preset)
                u8(notification.segments.size)
                for (segment in notification.segments) {
                    geometry(segment.geometry)
                    config(segment.config)
                }
            }
            is CatalogPage -> {
                u8(NotificationType.CATALOG)
                u8(notification.kind)
                u8(notification.start)
                u8(notification.total)
                u8(notification.entries.size)
                for (entry in notification.entries) {
                    u8(entry.id)
                    when (entry) {
                        is EffectInfo -> {
                            require(notification.kind == CatalogKind.EFFECTS) { "effect entry in a palette page" }
                            u8(entry.params.bits)
                        }
                        is PaletteInfo ->
                            require(notification.kind == CatalogKind.PALETTES) { "palette entry in an effect page" }
                    }
                    str(entry.name)
                }
            }
            is PresetList -> {
                u8(NotificationType.PRESETS)
                u8(notification.presets.size)
                for (preset in notification.presets) {
                    u8(preset.slot)
                    str(preset.name)
                }
            }
            is DeviceInfo -> {
                u8(NotificationType.INFO)
                u8(notification.proto)
                u8(notification.fwMajor)
                u8(notification.fwMinor)
                u8(notification.fwPatch)
                u8(notification.leds.size)
                notification.leds.forEach(::u16)
                u8(notification.maxSegments)
                u8(notification.presetSlots)
                u8(notification.effectCount)
                u8(notification.paletteCount)
                str(notification.build)
            }
            is ErrorReply -> {
                u8(NotificationType.ERROR)
                u8(notification.opcode)
                u8(notification.code)
            }
        }
    }.toByteArray()

    fun decodeNotification(frame: ByteArray): Notification {
        val r = FrameReader(frame)
        return when (val type = r.type) {
            NotificationType.STATE -> JacketState(
                proto = r.proto(),
                rev = r.u16(),
                power = r.u8() != 0,
                brightness = r.u8(),
                budgetMa = r.u16(),
                flags = r.u8(),
                preset = r.u8(),
                segments = List(r.u8()) { Segment(r.geometry(), r.config()) },
            )
            NotificationType.CATALOG -> {
                val kind = r.u8()
                val start = r.u8()
                val total = r.u8()
                val count = r.u8()
                val entries: List<CatalogEntry> = when (kind) {
                    CatalogKind.EFFECTS -> List(count) { EffectInfo(r.u8(), ParamFlags(r.u8()), r.str()) }
                    CatalogKind.PALETTES -> List(count) { PaletteInfo(r.u8(), r.str()) }
                    else -> throw FrameException(type, ErrorCode.OUT_OF_RANGE, "unknown catalogue kind $kind")
                }
                CatalogPage(kind, start, total, entries)
            }
            NotificationType.PRESETS -> PresetList(List(r.u8()) { PresetInfo(r.u8(), r.str()) })
            NotificationType.INFO -> DeviceInfo(
                proto = r.proto(),
                fwMajor = r.u8(),
                fwMinor = r.u8(),
                fwPatch = r.u8(),
                leds = List(r.u8()) { r.u16() },
                maxSegments = r.u8(),
                presetSlots = r.u8(),
                effectCount = r.u8(),
                paletteCount = r.u8(),
                build = r.str(),
            )
            NotificationType.ERROR -> ErrorReply(r.u8(), r.u8())
            else -> throw FrameException(type, ErrorCode.UNKNOWN_OPCODE, "unknown notification type $type")
        }
    }

    // ---- live characteristic ----------------------------------------------------------------------------------

    fun encode(live: LiveFrame): ByteArray = FrameWriter().apply {
        u8(live.flags)
        u8(live.seq)
        u8(live.level)
        u8(live.bass)
        u8(live.mid)
        u8(live.treble)
        u16(live.bpmX10)
        u8(live.beatPhase)
        u8(live.motion)
        i8(live.tiltX)
        i8(live.tiltY)
        u16(live.tMs)
    }.toByteArray()

    fun decodeLive(frame: ByteArray): LiveFrame {
        val r = FrameReader(frame, hasType = false)
        return LiveFrame(
            flags = r.u8(),
            seq = r.u8(),
            level = r.u8(),
            bass = r.u8(),
            mid = r.u8(),
            treble = r.u8(),
            bpmX10 = r.u16(),
            beatPhase = r.u8(),
            motion = r.u8(),
            tiltX = r.i8(),
            tiltY = r.i8(),
            tMs = r.u16(),
        )
    }
}

private class FrameWriter {
    private val buffer = ByteArray(Protocol.MAX_FRAME)
    private var size = 0

    fun u8(value: Int) {
        require(value in 0..0xFF) { "$value does not fit a u8" }
        put(value)
    }

    fun i8(value: Int) {
        require(value in -128..127) { "$value does not fit an i8" }
        put(value)
    }

    fun u16(value: Int) {
        require(value in 0..0xFFFF) { "$value does not fit a u16" }
        put(value)
        put(value shr 8)
    }

    fun bool(value: Boolean) = put(if (value) 1 else 0)

    fun rgb(value: Rgb) {
        put(value.r)
        put(value.g)
        put(value.b)
    }

    fun str(value: String) {
        val bytes = value.encodeToByteArray()
        u8(bytes.size)
        bytes.forEach { put(it.toInt()) }
    }

    fun config(value: EffectConfig) {
        u8(value.effect)
        u8(value.palette)
        u8(value.speed)
        u8(value.intensity)
        rgb(value.color1)
        rgb(value.color2)
    }

    fun geometry(value: SegmentGeometry) {
        u8(value.stripMask)
        u8(value.start)
        u8(value.length)
        u8(value.flags)
    }

    fun toByteArray(): ByteArray = buffer.copyOf(size)

    private fun put(value: Int) {
        require(size < buffer.size) { "frame longer than ${Protocol.MAX_FRAME} bytes" }
        buffer[size++] = value.toByte()
    }
}

private class FrameReader(private val frame: ByteArray, hasType: Boolean = true) {
    private var pos = 0

    /** The first byte of the frame: an opcode or a notification type. 0 for the type-less live frame. */
    val type: Int = if (hasType) u8() else 0

    fun u8(): Int {
        if (pos >= frame.size) tooShort()
        return frame[pos++].toInt() and 0xFF
    }

    fun i8(): Int = u8().toByte().toInt()

    fun u16(): Int = u8() or (u8() shl 8)

    /** Strict on purpose: a command carrying something other than 0/1 is a bug on the sending side. */
    fun bool(): Boolean = when (val value = u8()) {
        0 -> false
        1 -> true
        else -> throw FrameException(type, ErrorCode.OUT_OF_RANGE, "$value is not a boolean")
    }

    fun rgb(): Rgb = Rgb(u8(), u8(), u8())

    fun str(maxBytes: Int = 0xFF): String {
        val length = u8()
        if (length > maxBytes) throw FrameException(type, ErrorCode.OUT_OF_RANGE, "string of $length bytes, at most $maxBytes allowed")
        if (pos + length > frame.size) tooShort()
        return frame.decodeToString(pos, pos + length).also { pos += length }
    }

    fun config() = EffectConfig(u8(), u8(), u8(), u8(), rgb(), rgb())

    fun geometry() = SegmentGeometry(u8(), u8(), u8(), u8())

    fun proto(): Int = u8().also { if (it != Protocol.VERSION) throw UnsupportedProtocolException(type, it) }

    private fun tooShort(): Nothing = throw FrameException(
        type = if (frame.isEmpty()) 0 else frame[0].toInt() and 0xFF,
        code = ErrorCode.TOO_SHORT,
        message = "frame too short (${frame.size} bytes)",
    )
}
