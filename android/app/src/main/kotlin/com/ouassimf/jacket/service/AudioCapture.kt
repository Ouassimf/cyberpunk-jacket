package com.ouassimf.jacket.service

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTimestamp
import android.media.MediaRecorder
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.ouassimf.jacket.analysis.AudioAnalyzer
import com.ouassimf.jacket.live.LiveFeatures

/**
 * The microphone, read on its own thread and fed straight into an [AudioAnalyzer]. The samples live in one
 * buffer that is overwritten every 20 ms: nothing is stored, written to disk or sent anywhere. Only the
 * analyser's numbers leave this class, through [features].
 *
 * The voice-recognition source is asked for because Android requires it to come without automatic gain and
 * noise suppression, both of which would fight the analyser (and treat music as noise).
 */
internal class AudioCapture(private val features: LiveFeatures) {

    /** Passed on to the analyser before every block; set from any thread. */
    @Volatile
    var sensitivity = 50

    @Volatile
    private var running = false
    private var thread: Thread? = null

    /** Opens the microphone and starts analysing. False when it cannot be opened; nothing runs then. */
    fun start(): Boolean {
        if (thread != null) return true
        val record = open() ?: return false
        running = true
        thread = Thread({ run(record) }, "jacket-audio").apply { start() }
        return true
    }

    /** Stops and waits for the thread to let go of the microphone, which takes one read at most. */
    fun stop() {
        running = false
        thread?.join(STOP_TIMEOUT_MS)
        thread = null
    }

    @SuppressLint("MissingPermission") // the service only creates this once RECORD_AUDIO is granted
    private fun open(): AudioRecord? {
        val minimum = AudioRecord.getMinBufferSize(AudioAnalyzer.SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minimum <= 0) return null
        val bytes = maxOf(minimum, BLOCK * BYTES_PER_SAMPLE * BUFFERED_BLOCKS)
        for (source in SOURCES) {
            val record = try {
                AudioRecord(source, AudioAnalyzer.SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bytes)
            } catch (e: Exception) {
                Log.w(TAG, "audio source $source refused", e)
                continue
            }
            if (record.state == AudioRecord.STATE_INITIALIZED) return record
            record.release()
        }
        return null
    }

    private fun run(record: AudioRecord) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
        val analyzer = AudioAnalyzer()
        val block = ShortArray(BLOCK)
        val stamp = AudioTimestamp()
        var read = 0L
        try {
            record.startRecording()
            while (running) {
                val count = record.read(block, 0, BLOCK)
                if (count < 0) {
                    Log.w(TAG, "microphone read failed: $count")
                    break
                }
                if (count == 0) continue
                val now = SystemClock.elapsedRealtimeNanos()
                read += count
                analyzer.sensitivity = sensitivity
                analyzer.process(block, 0, count)
                features.audio(
                    atNanos = capturedAt(record, stamp, read, now),
                    level = analyzer.level,
                    bass = analyzer.bass,
                    mid = analyzer.mid,
                    treble = analyzer.treble,
                    beatValid = analyzer.beatValid,
                    bpm = analyzer.bpm,
                    beatPhase = analyzer.beatPhase,
                    beatPeriodSeconds = analyzer.beatPeriod,
                )
            }
        } catch (e: Exception) {
            Log.w(TAG, "microphone capture ended", e)
        } finally {
            try {
                record.stop()
            } catch (e: Exception) {
                // It never started; release below is all that is left to do.
            }
            record.release()
            features.clearAudio()
        }
    }

    /**
     * When the last sample read so far was captured, on the `elapsedRealtimeNanos` clock. The system knows this
     * better than the moment `read` returned, which is later by however long the sample sat in buffers; the
     * difference is what would otherwise make the jacket's beat late. Falls back to [now] when the system does
     * not say, or says something implausible.
     */
    private fun capturedAt(record: AudioRecord, stamp: AudioTimestamp, read: Long, now: Long): Long {
        if (record.getTimestamp(stamp, AudioTimestamp.TIMEBASE_BOOTTIME) != AudioRecord.SUCCESS) return now
        val captured = stamp.nanoTime + (read - stamp.framePosition) * NANOS_PER_SECOND / AudioAnalyzer.SAMPLE_RATE
        return if (captured in now - MAX_INPUT_DELAY_NANOS..now) captured else now
    }

    private companion object {
        const val TAG = "JacketAudio"

        /** Samples per read: 20 ms, two analysis frames. */
        const val BLOCK = 2 * AudioAnalyzer.HOP
        const val BYTES_PER_SAMPLE = 2
        const val BUFFERED_BLOCKS = 8
        const val STOP_TIMEOUT_MS = 500L
        const val NANOS_PER_SECOND = 1_000_000_000L
        const val MAX_INPUT_DELAY_NANOS = 500_000_000L
        val SOURCES = intArrayOf(MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC)
    }
}
