package com.ouassimf.jacket.link

import com.ouassimf.jacket.protocol.CatalogKind
import com.ouassimf.jacket.protocol.CatalogPage
import com.ouassimf.jacket.protocol.Codec
import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.LiveFrame
import com.ouassimf.jacket.protocol.Notification
import com.ouassimf.jacket.protocol.PresetList
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import java.io.IOException

/**
 * A link the test drives by hand: it decides the status, what the "jacket" notifies and when. Unlike the fake
 * jacket it can therefore misbehave: stay silent, answer late, report stale state or speak another protocol.
 */
class ScriptedLink : JacketLink {
    override val status = MutableStateFlow<LinkStatus>(LinkStatus.Disconnected)
    override val frames = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64)

    val writes = ArrayList<RecordingLink.Write>()
    val live = ArrayList<LiveFrame>()

    /** Runs before a live frame is taken, e.g. to make the write slow or fail. */
    var onLive: suspend () -> Unit = {}
    val priorities = ArrayList<Boolean>()
    var connects = 0
    var disconnects = 0
    var closed = false

    /** Answers the `GET_*` queries like a healthy jacket would. Replace to script something else. */
    var onCommand: (Command) -> Unit = ::answerQueries

    override fun connect() {
        connects++
    }

    override fun disconnect() {
        disconnects++
        status.value = LinkStatus.Disconnected
    }

    override suspend fun send(frame: ByteArray, withResponse: Boolean) {
        if (status.value != LinkStatus.Connected) throw IOException("not connected")
        val command = Codec.decodeCommand(frame)
        writes += RecordingLink.Write(command, withResponse)
        onCommand(command)
    }

    override suspend fun sendLive(frame: ByteArray) {
        if (status.value != LinkStatus.Connected) throw IOException("not connected")
        onLive()
        live += Codec.decodeLive(frame)
    }

    override fun setHighPriority(enabled: Boolean) {
        priorities += enabled
    }

    override fun close() {
        closed = true
        disconnect()
    }

    fun notify(notification: Notification) = raw(Codec.encode(notification))

    fun raw(frame: ByteArray) {
        check(frames.tryEmit(frame))
    }

    fun answerQueries(command: Command) {
        when (command) {
            Command.GetInfo -> notify(FakeJacket.INFO)
            is Command.GetCatalog -> {
                val all = if (command.kind == CatalogKind.EFFECTS) FakeJacket.EFFECTS else FakeJacket.PALETTES
                notify(CatalogPage(command.kind, command.start, all.size, all.drop(command.start)))
            }
            Command.GetPresets -> notify(PresetList(emptyList()))
            else -> Unit
        }
    }

    inline fun <reified T : Command> sent(): List<T> = writes.map { it.command }.filterIsInstance<T>()
}
