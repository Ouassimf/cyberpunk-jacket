package com.ouassimf.jacket.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import com.ouassimf.jacket.JacketApplication
import com.ouassimf.jacket.MainActivity
import com.ouassimf.jacket.R
import com.ouassimf.jacket.live.LiveController
import com.ouassimf.jacket.live.LiveNotice
import com.ouassimf.jacket.live.LiveSettings
import com.ouassimf.jacket.live.LiveStatus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Keeps live mode going with the screen off and the app in the background. It owns what only a foreground
 * service may hold there: the microphone, the accelerometer registration, a wake lock, and the notification
 * that says so and offers Stop. Everything else is the [LiveController]'s: the service starts it, follows its
 * status, and goes away when it turns off, whoever turned it off (the screen, the notification, or the
 * controller itself after the jacket stayed away too long).
 *
 * Foreground types, as Android 14 and later demand them:
 * - `microphone`, when sound is on and RECORD_AUDIO is granted. Being a while-in-use permission, this is why
 *   the service can only be started while the app is on screen; it is, the Start button lives there.
 * - `connectedDevice`, when BLUETOOTH_CONNECT is granted, for the link to the jacket.
 * With neither (demo mode on a phone that never allowed Bluetooth, motion only) it runs as a plain started
 * service: fine while the app is open, and Android will end it some time after the app leaves the screen.
 */
class LiveService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val live: LiveController get() = (application as JacketApplication).live

    private var begun = false
    private var finished = false
    private var foregroundTypes = 0
    private var audio: AudioCapture? = null
    private var motion: MotionCapture? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when {
            intent?.action == ACTION_STOP -> {
                live.stop()
                if (!begun) finish()
            }
            !begun -> begin()
            // Started again while running: a permission may have been granted since. Look again.
            else -> apply(live.settings.value)
        }
        return START_NOT_STICKY
    }

    private fun begin() {
        begun = true
        val settings = live.settings.value
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Live mode", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while the phone listens and streams to the jacket."
            },
        )
        if (!enterForeground(foregroundTypes(this, settings), LiveStatus.Waiting)) {
            live.stop(LiveNotice.StartRefused)
            finish()
            return
        }
        // The 30 Hz loop and the sensors need the CPU awake with the screen off. Released in finish(); the
        // timeout is only a safety net, live mode ends by itself long before.
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "jacket:live")
            .apply { acquire(WAKE_LOCK_LIMIT_MS) }

        live.start()
        if (!live.running) {
            finish()
            return
        }
        scope.launch { live.settings.collect(::apply) }
        scope.launch {
            live.status.collect { status ->
                if (status == LiveStatus.Off) finish() else notify(status)
            }
        }
    }

    /** Brings the microphone, the accelerometer and the foreground types in line with [settings] and the permissions. */
    private fun apply(settings: LiveSettings) {
        if (finished) return
        val types = foregroundTypes(this, settings)
        if (types != foregroundTypes) enterForeground(types, live.status.value)

        val listen = settings.sound && granted(this, Manifest.permission.RECORD_AUDIO)
        if (listen && audio == null) {
            val capture = AudioCapture(live.features)
            if (capture.start()) audio = capture else live.report(LiveNotice.MicrophoneUnavailable)
        } else if (!listen && audio != null) {
            audio?.stop()
            audio = null
        }
        audio?.sensitivity = settings.sensitivity

        if (settings.motion && motion == null) {
            val capture = MotionCapture(this, live.features)
            if (capture.start()) motion = capture
        } else if (!settings.motion && motion != null) {
            motion?.stop()
            motion = null
        }
        motion?.sensitivity = settings.sensitivity
    }

    /** False when Android refuses, which it does when the app is not on screen or a permission is missing. */
    private fun enterForeground(types: Int, status: LiveStatus): Boolean {
        if (types != 0) {
            try {
                startForeground(NOTIFICATION_ID, notification(status), types)
            } catch (e: Exception) {
                Log.w(TAG, "could not run in the foreground with types $types", e)
                return false
            }
        }
        foregroundTypes = types
        return true
    }

    private fun notify(status: LiveStatus) {
        if (foregroundTypes != 0) getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(status))
    }

    private fun notification(status: LiveStatus): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, LiveService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_live)
            .setContentTitle(if (status == LiveStatus.Streaming) "Live mode: streaming to the jacket" else "Live mode: waiting for the jacket")
            .setContentText("Sound is analysed on the phone. Nothing is recorded.")
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }

    private fun finish() {
        if (finished) return
        finished = true
        scope.cancel()
        audio?.stop()
        audio = null
        motion?.stop()
        motion = null
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        // Android took the service away (or finish() got here first): live mode must not go on without it.
        live.stop()
        finish()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "JacketLive"
        private const val CHANNEL = "live"
        private const val NOTIFICATION_ID = 1
        private const val WAKE_LOCK_LIMIT_MS = 12 * 60 * 60 * 1000L
        const val ACTION_STOP = "com.ouassimf.jacket.action.STOP_LIVE"

        /**
         * Starts live mode with the service around it. Call it while the app is on screen. Calling it again
         * while it runs makes the service look at the permissions again.
         */
        fun start(context: Context) {
            val settings = (context.applicationContext as JacketApplication).live.settings.value
            val intent = Intent(context, LiveService::class.java)
            try {
                if (foregroundTypes(context, settings) != 0) context.startForegroundService(intent) else context.startService(intent)
            } catch (e: Exception) {
                Log.w(TAG, "live mode could not be started", e)
                (context.applicationContext as JacketApplication).live.stop(LiveNotice.StartRefused)
            }
        }

        /** The foreground service types [settings] call for and the granted permissions allow; 0 for none. */
        fun foregroundTypes(context: Context, settings: LiveSettings): Int {
            var types = 0
            if (settings.sound && granted(context, Manifest.permission.RECORD_AUDIO)) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            if (granted(context, Manifest.permission.BLUETOOTH_CONNECT)) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            return types
        }

        private fun granted(context: Context, permission: String) = context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
    }
}
