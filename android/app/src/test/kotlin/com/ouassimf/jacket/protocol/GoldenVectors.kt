package com.ouassimf.jacket.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File

/**
 * `protocol/vectors/v1.json`, read from the repository itself (the build passes its path), and the mapping
 * between the JSON `fields` of a vector and this codec's types, in both directions. Field names are those of
 * PROTOCOL.md; a type this file does not know is an error, never a skip.
 */
class GoldenVector(val name: String, val channel: String, val type: String, val hex: String, val fields: JsonObject) {
    val bytes: ByteArray get() = hex.hexToBytes()

    /** The codec's value for [fields]: a [Command], a [Notification] or a [LiveFrame]. */
    fun frame(): Any = when (type) {
        "SET_POWER" -> Command.SetPower(flag("on"))
        "SET_BRIGHTNESS" -> Command.SetBrightness(int("value"))
        "SET_EFFECT" -> Command.SetEffect(int("segment"), config(fields.getValue("config").jsonObject))
        "SET_PARAM" -> {
            val value = fields.getValue("value")
            val param = when (val id = int("param")) {
                ParamId.EFFECT -> Param.Effect(value.jsonPrimitive.int)
                ParamId.PALETTE -> Param.Palette(value.jsonPrimitive.int)
                ParamId.SPEED -> Param.Speed(value.jsonPrimitive.int)
                ParamId.INTENSITY -> Param.Intensity(value.jsonPrimitive.int)
                ParamId.COLOR1 -> Param.Color1(rgb(value))
                ParamId.COLOR2 -> Param.Color2(rgb(value))
                else -> error("vector $name: unknown param $id")
            }
            Command.SetParam(int("segment"), param)
        }
        "SET_SEGMENTS" -> Command.SetSegments(fields.getValue("segments").jsonArray.map { geometry(it.jsonObject) })
        "IDENTIFY" -> Command.Identify(int("strip_mask"), int("start"), int("length"), int("seconds"))
        "PRESET_SAVE" -> Command.PresetSave(int("slot"), string("name"))
        "PRESET_LOAD" -> Command.PresetLoad(int("slot"))
        "PRESET_DELETE" -> Command.PresetDelete(int("slot"))
        "GET_PRESETS" -> Command.GetPresets
        "GET_INFO" -> Command.GetInfo
        "GET_CATALOG" -> Command.GetCatalog(int("kind"), int("start"))
        "SET_BUDGET" -> Command.SetBudget(int("milliamps"))
        "SET_DEV_MODE" -> Command.SetDevMode(flag("on"))
        "BOOT_OTHER_SLOT" -> Command.BootOtherSlot(int("confirm"))
        "STATE" -> JacketState(
            proto = int("proto"),
            rev = int("rev"),
            power = flag("power"),
            brightness = int("brightness"),
            budgetMa = int("budget_ma"),
            flags = int("flags"),
            preset = int("preset"),
            segments = fields.getValue("segments").jsonArray.map {
                Segment(geometry(it.jsonObject), config(it.jsonObject.getValue("config").jsonObject))
            },
        )
        "CATALOG" -> {
            val kind = int("kind")
            CatalogPage(
                kind = kind,
                start = int("start"),
                total = int("total"),
                entries = fields.getValue("entries").jsonArray.map { it.jsonObject }.map { entry ->
                    val id = entry.getValue("id").jsonPrimitive.int
                    val entryName = entry.getValue("name").jsonPrimitive.content
                    when (kind) {
                        CatalogKind.EFFECTS -> EffectInfo(id, ParamFlags(entry.getValue("param_flags").jsonPrimitive.int), entryName)
                        CatalogKind.PALETTES -> PaletteInfo(id, entryName)
                        else -> error("vector $name: unknown catalogue kind $kind")
                    }
                },
            )
        }
        "PRESETS" -> PresetList(
            fields.getValue("presets").jsonArray.map { it.jsonObject }.map {
                PresetInfo(it.getValue("slot").jsonPrimitive.int, it.getValue("name").jsonPrimitive.content)
            },
        )
        "INFO" -> DeviceInfo(
            proto = int("proto"),
            fwMajor = int("fw_major"),
            fwMinor = int("fw_minor"),
            fwPatch = int("fw_patch"),
            leds = fields.getValue("leds").jsonArray.map { it.jsonPrimitive.int },
            maxSegments = int("max_segments"),
            presetSlots = int("preset_slots"),
            effectCount = int("effect_count"),
            paletteCount = int("palette_count"),
            build = string("build"),
        )
        "ERROR" -> ErrorReply(int("opcode"), int("code"))
        "LIVE" -> LiveFrame(
            flags = int("flags"),
            seq = int("seq"),
            level = int("level"),
            bass = int("bass"),
            mid = int("mid"),
            treble = int("treble"),
            bpmX10 = int("bpm_x10"),
            beatPhase = int("beat_phase"),
            motion = int("motion"),
            tiltX = int("tilt_x"),
            tiltY = int("tilt_y"),
            tMs = int("t_ms"),
        )
        else -> error("vector $name: unknown type $type")
    }

    fun encode(): ByteArray = when (val frame = frame()) {
        is Command -> Codec.encode(frame)
        is Notification -> Codec.encode(frame)
        is LiveFrame -> Codec.encode(frame)
        else -> error("vector $name: nothing to encode")
    }

    fun decode(bytes: ByteArray = this.bytes): Any = when (channel) {
        "command" -> Codec.decodeCommand(bytes)
        "state" -> Codec.decodeNotification(bytes)
        "live" -> Codec.decodeLive(bytes)
        else -> error("vector $name: unknown channel $channel")
    }

    private fun int(field: String) = fields.getValue(field).jsonPrimitive.int
    private fun string(field: String) = fields.getValue(field).jsonPrimitive.content
    private fun flag(field: String) = when (val value = int(field)) {
        0 -> false
        1 -> true
        else -> error("vector $name: $field = $value is not 0/1")
    }

    override fun toString() = name

    companion object {
        /** Every vector of the file. Throws when the build did not say where the file is. */
        fun load(): List<GoldenVector> {
            val path = checkNotNull(System.getProperty("jacket.vectors")) { "system property jacket.vectors is not set" }
            val root = Json.parseToJsonElement(File(path).readText()).jsonObject
            check(root.getValue("protocol").jsonPrimitive.int == Protocol.VERSION) { "vectors are for another protocol version" }
            return root.getValue("vectors").jsonArray.map { it.jsonObject }.map {
                GoldenVector(
                    name = it.getValue("name").jsonPrimitive.content,
                    channel = it.getValue("channel").jsonPrimitive.content,
                    type = it.getValue("type").jsonPrimitive.content,
                    hex = it.getValue("hex").jsonPrimitive.content,
                    fields = it.getValue("fields").jsonObject,
                )
            }
        }

        /** The `fields` object a vector would carry for [frame]: the inverse of [GoldenVector.frame]. */
        fun fieldsOf(frame: Any): JsonObject = buildJsonObject {
            when (frame) {
                is Command.SetPower -> put("on", frame.on.toInt())
                is Command.SetBrightness -> put("value", frame.value)
                is Command.SetEffect -> {
                    put("segment", frame.segment)
                    put("config", config(frame.config))
                }
                is Command.SetParam -> {
                    put("segment", frame.segment)
                    put("param", frame.param.id)
                    when (val p = frame.param) {
                        is Param.Effect -> put("value", p.value)
                        is Param.Palette -> put("value", p.value)
                        is Param.Speed -> put("value", p.value)
                        is Param.Intensity -> put("value", p.value)
                        is Param.Color1 -> put("value", rgb(p.value))
                        is Param.Color2 -> put("value", rgb(p.value))
                    }
                }
                is Command.SetSegments -> put("segments", JsonArray(frame.segments.map { buildJsonObject { geometry(it) } }))
                is Command.Identify -> {
                    put("strip_mask", frame.stripMask)
                    put("start", frame.start)
                    put("length", frame.length)
                    put("seconds", frame.seconds)
                }
                is Command.PresetSave -> {
                    put("slot", frame.slot)
                    put("name", frame.name)
                }
                is Command.PresetLoad -> put("slot", frame.slot)
                is Command.PresetDelete -> put("slot", frame.slot)
                Command.GetPresets, Command.GetInfo -> Unit
                is Command.GetCatalog -> {
                    put("kind", frame.kind)
                    put("start", frame.start)
                }
                is Command.SetBudget -> put("milliamps", frame.milliamps)
                is Command.SetDevMode -> put("on", frame.on.toInt())
                is Command.BootOtherSlot -> put("confirm", frame.confirm)
                is JacketState -> {
                    put("proto", frame.proto)
                    put("rev", frame.rev)
                    put("power", frame.power.toInt())
                    put("brightness", frame.brightness)
                    put("budget_ma", frame.budgetMa)
                    put("flags", frame.flags)
                    put("preset", frame.preset)
                    put(
                        "segments",
                        JsonArray(
                            frame.segments.map {
                                buildJsonObject {
                                    geometry(it.geometry)
                                    put("config", config(it.config))
                                }
                            },
                        ),
                    )
                }
                is CatalogPage -> {
                    put("kind", frame.kind)
                    put("start", frame.start)
                    put("total", frame.total)
                    put(
                        "entries",
                        JsonArray(
                            frame.entries.map {
                                buildJsonObject {
                                    put("id", it.id)
                                    if (it is EffectInfo) put("param_flags", it.params.bits)
                                    put("name", it.name)
                                }
                            },
                        ),
                    )
                }
                is PresetList -> put(
                    "presets",
                    JsonArray(
                        frame.presets.map {
                            buildJsonObject {
                                put("slot", it.slot)
                                put("name", it.name)
                            }
                        },
                    ),
                )
                is DeviceInfo -> {
                    put("proto", frame.proto)
                    put("fw_major", frame.fwMajor)
                    put("fw_minor", frame.fwMinor)
                    put("fw_patch", frame.fwPatch)
                    put("leds", buildJsonArray { frame.leds.forEach { add(JsonPrimitive(it)) } })
                    put("max_segments", frame.maxSegments)
                    put("preset_slots", frame.presetSlots)
                    put("effect_count", frame.effectCount)
                    put("palette_count", frame.paletteCount)
                    put("build", frame.build)
                }
                is ErrorReply -> {
                    put("opcode", frame.opcode)
                    put("code", frame.code)
                }
                is LiveFrame -> {
                    put("flags", frame.flags)
                    put("seq", frame.seq)
                    put("level", frame.level)
                    put("bass", frame.bass)
                    put("mid", frame.mid)
                    put("treble", frame.treble)
                    put("bpm_x10", frame.bpmX10)
                    put("beat_phase", frame.beatPhase)
                    put("motion", frame.motion)
                    put("tilt_x", frame.tiltX)
                    put("tilt_y", frame.tiltY)
                    put("t_ms", frame.tMs)
                }
                else -> error("no vector fields for $frame")
            }
        }

        private fun config(json: JsonObject) = EffectConfig(
            effect = json.getValue("effect").jsonPrimitive.int,
            palette = json.getValue("palette").jsonPrimitive.int,
            speed = json.getValue("speed").jsonPrimitive.int,
            intensity = json.getValue("intensity").jsonPrimitive.int,
            color1 = rgb(json.getValue("color1")),
            color2 = rgb(json.getValue("color2")),
        )

        private fun config(config: EffectConfig) = buildJsonObject {
            put("effect", config.effect)
            put("palette", config.palette)
            put("speed", config.speed)
            put("intensity", config.intensity)
            put("color1", rgb(config.color1))
            put("color2", rgb(config.color2))
        }

        private fun geometry(json: JsonObject) = SegmentGeometry(
            stripMask = json.getValue("strip_mask").jsonPrimitive.int,
            start = json.getValue("start").jsonPrimitive.int,
            length = json.getValue("length").jsonPrimitive.int,
            flags = json.getValue("flags").jsonPrimitive.int,
        )

        private fun JsonObjectBuilder.geometry(geometry: SegmentGeometry) {
            put("strip_mask", geometry.stripMask)
            put("start", geometry.start)
            put("length", geometry.length)
            put("flags", geometry.flags)
        }

        private fun rgb(json: JsonElement): Rgb {
            val (r, g, b) = json.jsonArray.map { it.jsonPrimitive.int }.also { check(it.size == 3) { "rgb needs 3 values" } }
            return Rgb(r, g, b)
        }

        private fun rgb(rgb: Rgb) = buildJsonArray {
            add(JsonPrimitive(rgb.r))
            add(JsonPrimitive(rgb.g))
            add(JsonPrimitive(rgb.b))
        }

        private fun Boolean.toInt() = if (this) 1 else 0
    }
}

fun String.hexToBytes(): ByteArray {
    require(length % 2 == 0) { "odd hex length" }
    return ByteArray(length / 2) { substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}

fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }
