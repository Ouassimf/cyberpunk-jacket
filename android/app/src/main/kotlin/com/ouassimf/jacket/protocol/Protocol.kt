package com.ouassimf.jacket.protocol

/**
 * Constants of the jacket BLE protocol, v1. The contract is `protocol/PROTOCOL.md`; this package is its Kotlin
 * mirror and must stay free of Android imports (enforced by a unit test) so it can run anywhere.
 */
object Protocol {
    const val VERSION = 1

    const val DEVICE_NAME = "Jacket"
    const val SERVICE_UUID = "89960001-beff-438f-8710-5e7d3dff8891"
    const val COMMAND_UUID = "89960002-beff-438f-8710-5e7d3dff8891"
    const val STATE_UUID = "89960003-beff-438f-8710-5e7d3dff8891"
    const val LIVE_UUID = "89960004-beff-438f-8710-5e7d3dff8891"

    const val REQUESTED_MTU = 247
    const val MIN_MTU = 185
    const val MAX_FRAME = 180

    /** `segment` value addressing every segment at once. */
    const val ALL_SEGMENTS = 0xFF

    /** `preset` value in STATE when the scene matches no stored preset. */
    const val NO_PRESET = 0xFF

    const val MAX_SEGMENTS = 8

    /** `strip_mask` bits. */
    const val STRIP_LEFT = 0x01
    const val STRIP_RIGHT = 0x02

    /** Geometry `flags` bit 0: the segment runs from its last pixel to its first. */
    const val GEOMETRY_REVERSED = 0x01
    const val MAX_STRIP_LENGTH = 144
    const val PRESET_SLOTS = 8
    const val MAX_PRESET_NAME_BYTES = 16

    /** Not lower: the jacket draws roughly [IDLE_DRAW_MA] with every LED dark. */
    const val BUDGET_MIN_MA = 400
    const val BUDGET_MAX_MA = 2000
    const val BUDGET_DEFAULT_MA = 900

    /** About what the LEDs' idle current and the ESP32 take from any budget before the first LED lights. */
    const val IDLE_DRAW_MA = 370

    /** The only `confirm` value a `BOOT_OTHER_SLOT` command is accepted with. */
    const val BOOT_OTHER_SLOT_CONFIRM = 0x5A

    const val LIVE_FRAME_SIZE = 14
    const val LIVE_TIMEOUT_MS = 500L
}

object Opcode {
    const val SET_POWER = 0x01
    const val SET_BRIGHTNESS = 0x02
    const val SET_EFFECT = 0x03
    const val SET_PARAM = 0x04
    const val SET_SEGMENTS = 0x05
    const val IDENTIFY = 0x06
    const val PRESET_SAVE = 0x10
    const val PRESET_LOAD = 0x11
    const val PRESET_DELETE = 0x12
    const val GET_PRESETS = 0x13
    const val GET_INFO = 0x20
    const val GET_CATALOG = 0x21
    const val SET_BUDGET = 0x30
    const val SET_DEV_MODE = 0x31
    const val BOOT_OTHER_SLOT = 0x32
}

object NotificationType {
    const val STATE = 0x01
    const val CATALOG = 0x02
    const val PRESETS = 0x04
    const val INFO = 0x05
    const val ERROR = 0x7F
}

object ErrorCode {
    const val UNKNOWN_OPCODE = 1
    const val TOO_SHORT = 2
    const val OUT_OF_RANGE = 3

    /** `PRESET_LOAD` or `PRESET_DELETE` on an empty slot. */
    const val NOT_FOUND = 4

    /** The jacket could not write to its storage. */
    const val STORAGE_FAILURE = 5
}

object ParamId {
    const val EFFECT = 1
    const val PALETTE = 2
    const val SPEED = 3
    const val INTENSITY = 4
    const val COLOR1 = 5
    const val COLOR2 = 6
}

object CatalogKind {
    const val EFFECTS = 0
    const val PALETTES = 1
}
