package com.ouassimf.jacket.data

import com.ouassimf.jacket.protocol.Command
import com.ouassimf.jacket.protocol.JacketState
import com.ouassimf.jacket.protocol.Param
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.Segment
import com.ouassimf.jacket.protocol.has
import com.ouassimf.jacket.protocol.with

/**
 * One thing the user changes on the jacket. A change knows the command that makes it, what the jacket's state
 * looks like once it is applied, and which control it comes from, which is all the repository needs to send it,
 * show it optimistically and recognise the jacket's confirmation.
 */
sealed interface Change {
    data class Power(val on: Boolean) : Change
    data class Brightness(val value: Int) : Change
    data class Budget(val milliamps: Int) : Change
    data class DevMode(val on: Boolean) : Change

    /** [segment] is a segment index, or [Protocol.ALL_SEGMENTS]. */
    data class SegmentParam(val segment: Int, val param: Param) : Change

    /** A whole new set of segments, each with the effect it should play. See [layoutCommands]. */
    data class Layout(val segments: List<Segment>) : Change
}

/** Identifies the control behind a change: a newer change with the same key replaces an older one. */
internal val Change.key: Int
    get() = when (this) {
        is Change.Power -> 1
        is Change.Brightness -> 2
        is Change.Budget -> 3
        is Change.DevMode -> 4
        is Change.Layout -> 5
        is Change.SegmentParam -> 0x10000 or (segment shl 8) or param.id
    }

/** The commands that make this change on a jacket currently in [state], in the order to send them. */
internal fun Change.toCommands(state: JacketState): List<Command> = when (this) {
    is Change.Power -> listOf(Command.SetPower(on))
    is Change.Brightness -> listOf(Command.SetBrightness(value))
    is Change.Budget -> listOf(Command.SetBudget(milliamps))
    is Change.DevMode -> listOf(Command.SetDevMode(on))
    is Change.SegmentParam -> listOf(Command.SetParam(segment, param))
    is Change.Layout -> layoutCommands(state.segments, segments)
}

/**
 * `SET_SEGMENTS` only carries geometry: segment *i* keeps the effect segment *i* had, and segments beyond the
 * old count copy segment 0. When zones were removed or reordered that is not what the user laid out, so the
 * effects that would end up on the wrong zone are put right with one `SET_EFFECT` each.
 */
internal fun layoutCommands(old: List<Segment>, new: List<Segment>): List<Command> = buildList {
    add(Command.SetSegments(new.map { it.geometry }))
    new.forEachIndexed { i, segment ->
        val inherited = (old.getOrNull(i) ?: old[0]).config
        if (segment.config != inherited) add(Command.SetEffect(i, segment.config))
    }
}

internal fun Change.applyTo(state: JacketState): JacketState = when (this) {
    is Change.Power -> state.copy(power = on)
    is Change.Brightness -> state.copy(brightness = value)
    is Change.Budget -> state.copy(budgetMa = milliamps)
    is Change.DevMode -> state.copy(
        flags = if (on) state.flags or JacketState.FLAG_DEV_MODE else state.flags and JacketState.FLAG_DEV_MODE.inv(),
    )
    is Change.SegmentParam -> state.copy(
        segments = state.segments.mapIndexed { i, s -> if (targets(i)) s.copy(config = s.config.with(param)) else s },
    )
    is Change.Layout -> state.copy(segments = segments)
}

/** True once [state], as reported by the jacket, already shows this change. */
internal fun Change.isReflectedIn(state: JacketState): Boolean = when (this) {
    is Change.Power -> state.power == on
    is Change.Brightness -> state.brightness == value
    is Change.Budget -> state.budgetMa == milliamps
    is Change.DevMode -> state.devMode == on
    is Change.SegmentParam -> state.segments.withIndex().all { (i, s) -> !targets(i) || s.config.has(param) }
    is Change.Layout -> state.segments == segments
}

private fun Change.SegmentParam.targets(index: Int) = segment == Protocol.ALL_SEGMENTS || segment == index
