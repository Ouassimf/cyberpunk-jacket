package com.ouassimf.jacket.protocol

/** A colour as it travels on the wire (red, green, blue), packed as `0xRRGGBB` so it never allocates. */
@JvmInline
value class Rgb(val packed: Int) {
    constructor(r: Int, g: Int, b: Int) : this((r shl 16) or (g shl 8) or b) {
        require(r in 0..255 && g in 0..255 && b in 0..255) { "rgb($r, $g, $b) out of range" }
    }

    val r: Int get() = (packed shr 16) and 0xFF
    val g: Int get() = (packed shr 8) and 0xFF
    val b: Int get() = packed and 0xFF

    override fun toString(): String = "#%06X".format(packed and 0xFFFFFF)
}

data class EffectConfig(
    val effect: Int,
    val palette: Int,
    /** Time multiplier is `2^((speed − 128) / 32)`; 128 is 1×. */
    val speed: Int,
    val intensity: Int,
    val color1: Rgb,
    val color2: Rgb,
)

data class SegmentGeometry(
    /** Bit 0 = left strip, bit 1 = right strip. */
    val stripMask: Int,
    val start: Int,
    val length: Int,
    /** Bit 0 = reversed. */
    val flags: Int,
) {
    val reversed: Boolean get() = flags and Protocol.GEOMETRY_REVERSED != 0

    /** One past the last pixel. */
    val end: Int get() = start + length
}

data class Segment(val geometry: SegmentGeometry, val config: EffectConfig)

/** Which controls an effect uses, as declared by the jacket in its catalogue. */
@JvmInline
value class ParamFlags(val bits: Int) {
    val color1: Boolean get() = bits and 0x01 != 0
    val color2: Boolean get() = bits and 0x02 != 0
    val palette: Boolean get() = bits and 0x04 != 0
    val speed: Boolean get() = bits and 0x08 != 0
    val intensity: Boolean get() = bits and 0x10 != 0
    val sound: Boolean get() = bits and 0x20 != 0
    val motion: Boolean get() = bits and 0x40 != 0
}

/** One value of `SET_PARAM`; the subclass is the parameter id. */
sealed interface Param {
    data class Effect(val value: Int) : Param
    data class Palette(val value: Int) : Param
    data class Speed(val value: Int) : Param
    data class Intensity(val value: Int) : Param
    data class Color1(val value: Rgb) : Param
    data class Color2(val value: Rgb) : Param

    val id: Int
        get() = when (this) {
            is Effect -> ParamId.EFFECT
            is Palette -> ParamId.PALETTE
            is Speed -> ParamId.SPEED
            is Intensity -> ParamId.INTENSITY
            is Color1 -> ParamId.COLOR1
            is Color2 -> ParamId.COLOR2
        }
}

fun EffectConfig.with(param: Param): EffectConfig = when (param) {
    is Param.Effect -> copy(effect = param.value)
    is Param.Palette -> copy(palette = param.value)
    is Param.Speed -> copy(speed = param.value)
    is Param.Intensity -> copy(intensity = param.value)
    is Param.Color1 -> copy(color1 = param.value)
    is Param.Color2 -> copy(color2 = param.value)
}

fun EffectConfig.has(param: Param): Boolean = when (param) {
    is Param.Effect -> effect == param.value
    is Param.Palette -> palette == param.value
    is Param.Speed -> speed == param.value
    is Param.Intensity -> intensity == param.value
    is Param.Color1 -> color1 == param.value
    is Param.Color2 -> color2 == param.value
}

/** Frames written to the `command` characteristic. */
sealed interface Command {
    data class SetPower(val on: Boolean) : Command
    data class SetBrightness(val value: Int) : Command
    data class SetEffect(val segment: Int, val config: EffectConfig) : Command
    data class SetParam(val segment: Int, val param: Param) : Command
    data class SetSegments(val segments: List<SegmentGeometry>) : Command
    data class Identify(val stripMask: Int, val start: Int, val length: Int, val seconds: Int) : Command
    data class PresetSave(val slot: Int, val name: String) : Command
    data class PresetLoad(val slot: Int) : Command
    data class PresetDelete(val slot: Int) : Command
    data object GetPresets : Command
    data object GetInfo : Command
    data class GetCatalog(val kind: Int, val start: Int) : Command
    data class SetBudget(val milliamps: Int) : Command
    data class SetDevMode(val on: Boolean) : Command

    /**
     * Restarts the jacket into the firmware in its other app slot: the one that ran before the last update.
     * [confirm] must be [Protocol.BOOT_OTHER_SLOT_CONFIRM], so that a stray write cannot trigger it.
     */
    data class BootOtherSlot(val confirm: Int = Protocol.BOOT_OTHER_SLOT_CONFIRM) : Command

    val opcode: Int
        get() = when (this) {
            is SetPower -> Opcode.SET_POWER
            is SetBrightness -> Opcode.SET_BRIGHTNESS
            is SetEffect -> Opcode.SET_EFFECT
            is SetParam -> Opcode.SET_PARAM
            is SetSegments -> Opcode.SET_SEGMENTS
            is Identify -> Opcode.IDENTIFY
            is PresetSave -> Opcode.PRESET_SAVE
            is PresetLoad -> Opcode.PRESET_LOAD
            is PresetDelete -> Opcode.PRESET_DELETE
            GetPresets -> Opcode.GET_PRESETS
            GetInfo -> Opcode.GET_INFO
            is GetCatalog -> Opcode.GET_CATALOG
            is SetBudget -> Opcode.SET_BUDGET
            is SetDevMode -> Opcode.SET_DEV_MODE
            is BootOtherSlot -> Opcode.BOOT_OTHER_SLOT
        }
}

/** Frames notified on (or read from) the `state` characteristic. */
sealed interface Notification

data class JacketState(
    val proto: Int,
    /** Increments on every change and wraps at 65535: compare for equality only, never for order. */
    val rev: Int,
    val power: Boolean,
    val brightness: Int,
    val budgetMa: Int,
    val flags: Int,
    /** Slot last loaded or saved while the scene is unchanged, else [Protocol.NO_PRESET]. */
    val preset: Int,
    val segments: List<Segment>,
) : Notification {
    val devMode: Boolean get() = flags and FLAG_DEV_MODE != 0
    val wifiConnected: Boolean get() = flags and FLAG_WIFI != 0
    val liveInput: Boolean get() = flags and FLAG_LIVE != 0

    companion object {
        const val FLAG_DEV_MODE = 0x01
        const val FLAG_WIFI = 0x02
        const val FLAG_LIVE = 0x04
    }
}

sealed interface CatalogEntry {
    val id: Int
    val name: String
}

data class EffectInfo(override val id: Int, val params: ParamFlags, override val name: String) : CatalogEntry
data class PaletteInfo(override val id: Int, override val name: String) : CatalogEntry

/** One page of a catalogue. [entries] are all [EffectInfo] or all [PaletteInfo], according to [kind]. */
data class CatalogPage(
    val kind: Int,
    val start: Int,
    val total: Int,
    val entries: List<CatalogEntry>,
) : Notification

data class PresetInfo(val slot: Int, val name: String)
data class PresetList(val presets: List<PresetInfo>) : Notification

data class DeviceInfo(
    val proto: Int,
    val fwMajor: Int,
    val fwMinor: Int,
    val fwPatch: Int,
    /** LED count of each strip; the list size is `strip_count`. */
    val leds: List<Int>,
    val maxSegments: Int,
    val presetSlots: Int,
    val effectCount: Int,
    val paletteCount: Int,
    val build: String,
) : Notification {
    val version: String get() = "$fwMajor.$fwMinor.$fwPatch"
}

data class ErrorReply(val opcode: Int, val code: Int) : Notification

/** The 14-byte frame written to the `live` characteristic (draft, used from phase 6). It has no type byte. */
data class LiveFrame(
    val flags: Int,
    val seq: Int,
    val level: Int,
    val bass: Int,
    val mid: Int,
    val treble: Int,
    val bpmX10: Int,
    val beatPhase: Int,
    val motion: Int,
    val tiltX: Int,
    val tiltY: Int,
    val tMs: Int,
) {
    companion object {
        const val FLAG_AUDIO = 0x01
        const val FLAG_MOTION = 0x02
        const val FLAG_BEAT = 0x04
    }
}
