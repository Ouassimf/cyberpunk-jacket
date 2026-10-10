package com.ouassimf.jacket.analysis

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

/**
 * An in-place radix-2 FFT of one fixed power-of-two [size]. The tables are built once, so a transform allocates
 * nothing. Small on purpose: at 512 points and 100 transforms a second it is a few thousand multiplications
 * each, which a phone does not notice.
 */
internal class Fft(val size: Int) {
    private val cosTable = FloatArray(size / 2)
    private val sinTable = FloatArray(size / 2)
    private val reversed = IntArray(size)

    init {
        require(size >= 2 && size and (size - 1) == 0) { "$size is not a power of two" }
        for (i in 0 until size / 2) {
            cosTable[i] = cos(2.0 * PI * i / size).toFloat()
            sinTable[i] = sin(2.0 * PI * i / size).toFloat()
        }
        val bits = Integer.numberOfTrailingZeros(size)
        for (i in 0 until size) reversed[i] = Integer.reverse(i) ushr (32 - bits)
    }

    /** Replaces [re] and [im] by their forward transform (no scaling). */
    fun transform(re: FloatArray, im: FloatArray) {
        for (i in 0 until size) {
            val j = reversed[i]
            if (j > i) {
                val r = re[i]; re[i] = re[j]; re[j] = r
                val m = im[i]; im[i] = im[j]; im[j] = m
            }
        }
        var half = 1
        while (half < size) {
            val step = size / (half * 2)
            var start = 0
            while (start < size) {
                var k = 0
                for (i in start until start + half) {
                    val wr = cosTable[k]
                    val wi = -sinTable[k]
                    val j = i + half
                    val tr = re[j] * wr - im[j] * wi
                    val ti = re[j] * wi + im[j] * wr
                    re[j] = re[i] - tr
                    im[j] = im[i] - ti
                    re[i] += tr
                    im[i] += ti
                    k += step
                }
                start += half * 2
            }
            half *= 2
        }
    }
}
