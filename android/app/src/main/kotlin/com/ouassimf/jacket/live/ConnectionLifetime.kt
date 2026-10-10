package com.ouassimf.jacket.live

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.time.Duration

/**
 * Decides how long the app holds on to the jacket when nobody is looking at it.
 *
 * The jacket takes one central at a time and a held connection costs battery on both sides, so once the screens
 * are gone the app lets go after [grace] (not at once: a quick trip to another app should not cost a
 * reconnect). Live mode is the exception: while it runs the connection is its business, however long the screen
 * stays off. When live mode ends with the screens still gone, the grace period starts then.
 *
 * Call it from [scope]'s single-threaded dispatcher.
 */
class ConnectionLifetime(private val scope: CoroutineScope, private val grace: Duration, private val disconnect: () -> Unit) {
    private var uiVisible = false
    private var liveRunning = false
    private var pending: Job? = null

    fun onUiStarted() {
        uiVisible = true
        update()
    }

    fun onUiStopped() {
        uiVisible = false
        update()
    }

    fun onLiveChanged(running: Boolean) {
        if (running == liveRunning) return
        liveRunning = running
        update()
    }

    private fun update() {
        if (uiVisible || liveRunning) {
            pending?.cancel()
            pending = null
        } else if (pending == null) {
            // Stays set once it has fired: there is nothing more to let go of until someone comes back.
            pending = scope.launch {
                delay(grace)
                disconnect()
            }
        }
    }
}
