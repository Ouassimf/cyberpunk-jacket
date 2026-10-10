package com.ouassimf.jacket

import android.app.Application
import android.os.SystemClock
import com.ouassimf.jacket.ble.BleJacketLink
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.link.FakeJacket
import com.ouassimf.jacket.live.ConnectionLifetime
import com.ouassimf.jacket.live.LiveController
import com.ouassimf.jacket.live.LiveFeatures
import com.ouassimf.jacket.live.LiveStatus
import com.ouassimf.jacket.service.LivePreferences
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlin.time.Duration.Companion.seconds

/**
 * Holds the one [JacketRepository] and the one [LiveController] for the life of the process, so the connection
 * and a live session survive rotation and the activity being recreated. There is no dependency injection: this
 * class is the whole object graph.
 */
class JacketApplication : Application() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    lateinit var repository: JacketRepository
        private set

    /** Live mode. The foreground service (`service.LiveService`) runs it; the Live screen shows and steers it. */
    lateinit var live: LiveController
        private set

    private lateinit var lifetime: ConnectionLifetime

    override fun onCreate() {
        super.onCreate()
        val preferences = getSharedPreferences("settings", MODE_PRIVATE)
        repository = JacketRepository(
            scope = scope,
            linkFactory = { demo -> if (demo) FakeJacket(scope) else BleJacketLink(scope) },
            demoMode = preferences.getBoolean(KEY_DEMO_MODE, false),
        )
        // The clock the capture classes stamp their results with: monotonic, and the one sensors and audio
        // timestamps use.
        live = LiveController(scope, repository, LiveFeatures(), LivePreferences.load(preferences), SystemClock::elapsedRealtimeNanos)
        lifetime = ConnectionLifetime(scope, BACKGROUND_GRACE) { repository.disconnect() }
        scope.launch {
            repository.demoMode.drop(1).collect { preferences.edit().putBoolean(KEY_DEMO_MODE, it).apply() }
        }
        scope.launch {
            live.settings.drop(1).collect { LivePreferences.save(preferences, it) }
        }
        scope.launch {
            live.status.collect { lifetime.onLiveChanged(it != LiveStatus.Off) }
        }
    }

    /** The UI is visible again: keep whatever connection there is. Connecting itself is the UI's call. */
    fun onUiStarted() = lifetime.onUiStarted()

    /**
     * The UI is gone. The jacket takes one central at a time and a held connection costs battery on both sides,
     * so let go, but not for a quick trip to another app, and not while live mode streams in the background:
     * see [ConnectionLifetime].
     */
    fun onUiStopped() = lifetime.onUiStopped()

    private companion object {
        const val KEY_DEMO_MODE = "demo_mode"
        val BACKGROUND_GRACE = 20.seconds
    }
}
