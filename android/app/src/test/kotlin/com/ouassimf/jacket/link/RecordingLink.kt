package com.ouassimf.jacket.link

import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.LiveFrame

/** Passes everything to [delegate] and remembers what was written, so tests can assert on the wire traffic. */
class RecordingLink(private val delegate: JacketLink) : JacketLink by delegate {

    data class Write(val command: Command, val withResponse: Boolean)

    val writes = ArrayList<Write>()

    /** Every frame written to the `live` characteristic, decoded. */
    val live = ArrayList<LiveFrame>()
    val priorities = ArrayList<Boolean>()
    var connects = 0
    var disconnects = 0

    /** Return true to lose the frame on the way to the jacket. */
    var drop: (Command) -> Boolean = { false }

    override fun connect() {
        connects++
        delegate.connect()
    }

    override fun disconnect() {
        disconnects++
        delegate.disconnect()
    }

    override suspend fun send(frame: ByteArray, withResponse: Boolean) {
        val command = Codec.decodeCommand(frame)
        writes += Write(command, withResponse)
        if (!drop(command)) delegate.send(frame, withResponse)
    }

    override suspend fun sendLive(frame: ByteArray) {
        delegate.sendLive(frame) // first: a frame the jacket could not take was not sent
        live += Codec.decodeLive(frame)
    }

    override fun setHighPriority(enabled: Boolean) {
        priorities += enabled
        delegate.setHighPriority(enabled)
    }

    inline fun <reified T : Command> sent(): List<T> = writes.map { it.command }.filterIsInstance<T>()
}
