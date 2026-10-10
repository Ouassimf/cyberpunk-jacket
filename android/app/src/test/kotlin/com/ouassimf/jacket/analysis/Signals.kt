package com.ouassimf.jacket.analysis

import kotlin.math.PI
import kotlin.math.exp
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.random.Random

/** Synthetic sound for the analyser tests: a mix is built in floats (1.0 is full scale) and rendered to PCM. */
internal class Mix(val seconds: Double) {
    val rate = AudioAnalyzer.SAMPLE_RATE
    val data = FloatArray((seconds * rate).roundToInt())

    fun tone(hz: Double, amplitude: Double, from: Double = 0.0, to: Double = seconds) = apply {
        for (i in index(from) until index(to)) data[i] += (amplitude * sin(2 * PI * hz * i / rate)).toFloat()
    }

    fun noise(amplitude: Double, random: Random, from: Double = 0.0, to: Double = seconds) = apply {
        for (i in index(from) until index(to)) data[i] += (amplitude * (random.nextDouble() * 2 - 1)).toFloat()
    }

    /** A kick drum at [at] seconds: a sine sweeping down from 150 to 50 Hz under a short click, about 150 ms long. */
    fun kick(at: Double, amplitude: Double, random: Random) = apply {
        val start = at * rate
        var i = maxOf(0, kotlin.math.ceil(start).toInt())
        var phase = 0.0
        while (i < data.size) {
            val t = (i - start) / rate
            if (t > 0.25) break
            phase += 2 * PI * (50 + 100 * exp(-t / 0.02)) / rate
            val body = exp(-t / 0.05) * sin(phase)
            val click = 0.5 * exp(-t / 0.003) * (random.nextDouble() * 2 - 1)
            data[i] += (amplitude * (body + click)).toFloat()
            i++
        }
    }

    /** A closed hi-hat at [at] seconds: 30 ms of bright noise. */
    fun hat(at: Double, amplitude: Double, random: Random) = apply {
        val start = at * rate
        var i = maxOf(0, kotlin.math.ceil(start).toInt())
        var last = 0.0
        while (i < data.size) {
            val t = (i - start) / rate
            if (t > 0.1) break
            val white = random.nextDouble() * 2 - 1
            data[i] += (amplitude * exp(-t / 0.012) * (white - last)).toFloat() // differenced: mostly treble
            last = white
            i++
        }
    }

    /** Kicks on every beat of [bpm] from [first] seconds on, optionally with hats half-way between them. */
    fun beat(
        bpm: Double,
        random: Random,
        first: Double = 0.2,
        from: Double = 0.0,
        to: Double = seconds,
        amplitude: Double = 0.5,
        hats: Double = 0.0,
        jitter: Double = 0.0,
    ) = apply {
        val period = 60 / bpm
        var at = first
        while (at < to) {
            if (at >= from) {
                kick(at + jitter * (random.nextDouble() * 2 - 1), amplitude, random)
                if (hats > 0 && at + period / 2 < to) hat(at + period / 2 + jitter * (random.nextDouble() * 2 - 1), hats, random)
            }
            at += period
        }
    }

    fun scale(factor: Double, from: Double = 0.0, to: Double = seconds) = apply {
        for (i in index(from) until index(to)) data[i] = (data[i] * factor).toFloat()
    }

    fun pcm(): ShortArray = ShortArray(data.size) { (data[it] * 32767f).roundToInt().coerceIn(-32768, 32767).toShort() }

    private fun index(seconds: Double) = (seconds * rate).roundToInt().coerceIn(0, data.size)
}

/** Feeds [pcm] in blocks of [block] samples, the way a microphone would, calling [afterBlock] with the time so far. */
internal fun AudioAnalyzer.feed(pcm: ShortArray, block: Int = 320, afterBlock: (seconds: Double) -> Unit = {}) {
    var offset = 0
    while (offset < pcm.size) {
        val count = minOf(block, pcm.size - offset)
        process(pcm, offset, count)
        offset += count
        afterBlock(offset.toDouble() / AudioAnalyzer.SAMPLE_RATE)
    }
}
