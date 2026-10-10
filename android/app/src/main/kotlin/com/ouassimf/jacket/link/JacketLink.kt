package com.ouassimf.jacket.link

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

/**
 * The transport to one jacket, at the level of whole protocol frames. It knows nothing about what the frames
 * mean; `JacketRepository` does. Two implementations: Bluetooth LE, and an in-app fake for demo mode and tests.
 *
 * Contract:
 * - [connect] keeps trying until [disconnect]: a link that drops goes back to [LinkStatus.Connecting] and
 *   reconnects by itself. Only [LinkStatus.Failed] is terminal, until the next [connect].
 * - Every time the link becomes [LinkStatus.Connected] it has already subscribed to `state` and emitted the
 *   frame read from it, so a collector of [frames] always sees a fresh STATE first.
 * - [send] and [sendLive] throw [java.io.IOException] when the frame could not be handed to the jacket.
 */
interface JacketLink {
    val status: StateFlow<LinkStatus>

    /** Frames notified on (or read from) the `state` characteristic. Hot: collect before calling [connect]. */
    val frames: Flow<ByteArray>

    fun connect()

    fun disconnect()

    /** Writes one frame to `command`, acknowledged by the jacket when [withResponse] is true. */
    suspend fun send(frame: ByteArray, withResponse: Boolean)

    /** Writes one frame to `live`, never acknowledged. */
    suspend fun sendLive(frame: ByteArray)

    /**
     * Asks for a short connection interval, for as long as a control is dragged or live mode streams; it costs
     * battery on both sides, so it is given back as soon as neither does.
     */
    fun setHighPriority(enabled: Boolean)

    /** Releases everything. The link cannot be used afterwards. */
    fun close()
}

sealed interface LinkStatus {
    data object Disconnected : LinkStatus
    data object Scanning : LinkStatus
    data object Connecting : LinkStatus
    data object Connected : LinkStatus
    data class Failed(val reason: LinkFailure) : LinkStatus
}

enum class LinkFailure {
    BluetoothOff,
    PermissionMissing,

    /** The negotiated ATT MTU is below what the protocol needs; the spec forbids using such a link. */
    MtuTooLow,
}
