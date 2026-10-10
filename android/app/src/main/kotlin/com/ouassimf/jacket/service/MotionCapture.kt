package com.ouassimf.jacket.service

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import com.ouassimf.jacket.analysis.MotionAnalyzer
import com.ouassimf.jacket.live.LiveFeatures

/**
 * The accelerometer, delivered on its own thread at the "game" rate (about 50 samples a second) and fed straight
 * into a [MotionAnalyzer]. Only the analyser's numbers leave this class, through [features].
 */
internal class MotionCapture(context: Context, private val features: LiveFeatures) : SensorEventListener {

    /** Passed on to the analyser with every sample; set from any thread. */
    @Volatile
    var sensitivity = 50

    private val manager = context.getSystemService(SensorManager::class.java)
    private val analyzer = MotionAnalyzer()
    private var thread: HandlerThread? = null

    /** False when the phone has no accelerometer or will not deliver it; nothing runs then. */
    fun start(): Boolean {
        if (thread != null) return true
        val sensor = manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return false
        val worker = HandlerThread("jacket-motion").apply { start() }
        if (!manager.registerListener(this, sensor, SensorManager.SENSOR_DELAY_GAME, Handler(worker.looper))) {
            worker.quitSafely()
            return false
        }
        thread = worker
        return true
    }

    fun stop() {
        val worker = thread ?: return
        manager?.unregisterListener(this)
        worker.quitSafely()
        thread = null
        features.clearMotion()
    }

    override fun onSensorChanged(event: SensorEvent) {
        analyzer.sensitivity = sensitivity
        analyzer.add(event.values[0], event.values[1], event.values[2], event.timestamp)
        features.motion(SystemClock.elapsedRealtimeNanos(), analyzer.motion, analyzer.tiltX, analyzer.tiltY)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
}
