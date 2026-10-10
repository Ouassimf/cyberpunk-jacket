package com.ouassimf.jacket.ui

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.LifecycleResumeEffect

/** Where a runtime permission stands. */
enum class Permit {
    Granted,

    /** Not granted yet; the system dialog can still be shown. */
    Needed,

    /** Denied for good: only the app's page in system settings can change it. */
    Blocked,
}

/**
 * The two permissions live mode asks for: the microphone, without which it cannot listen, and notifications,
 * without which it still runs but cannot show its Stop button outside the app (Android 13 and later).
 */
@Stable
class LiveGate internal constructor(private val context: Context) {

    var microphone: Permit by mutableStateOf(Permit.Needed)
        private set

    var notifications: Permit by mutableStateOf(Permit.Granted)
        private set

    private val blocked = HashSet<String>()
    private var afterAnswer: (() -> Unit)? = null
    internal var launch: (Array<String>) -> Unit = {}

    init {
        refresh()
    }

    /** Shows the system dialog for [permissions], then runs [then], whatever the answer was. */
    fun request(permissions: List<String>, then: () -> Unit = {}) {
        afterAnswer = then
        launch(permissions.toTypedArray())
    }

    /** Opens the app's page in the system settings, the only place a blocked permission can be granted. */
    fun openSettings() {
        context.startActivity(
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }

    internal fun onResult(result: Map<String, Boolean>) {
        // Once the system stops offering the dialog, a denial comes straight back, with no rationale to show.
        val activity = context.findActivity()
        for ((permission, granted) in result) {
            if (!granted && activity != null && !activity.shouldShowRequestPermissionRationale(permission)) blocked += permission
        }
        refresh()
        afterAnswer?.invoke()
        afterAnswer = null
    }

    internal fun refresh() {
        microphone = permit(Manifest.permission.RECORD_AUDIO)
        notifications = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) permit(Manifest.permission.POST_NOTIFICATIONS) else Permit.Granted
    }

    private fun permit(permission: String): Permit = when {
        context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED -> {
            blocked -= permission
            Permit.Granted
        }
        permission in blocked -> Permit.Blocked
        else -> Permit.Needed
    }
}

@Composable
fun rememberLiveGate(): LiveGate {
    val context = LocalContext.current
    val gate = remember(context) { LiveGate(context) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions(), gate::onResult)
    gate.launch = { launcher.launch(it) }

    // Permissions can change in system settings while the app is paused.
    LifecycleResumeEffect(gate) {
        gate.refresh()
        onPauseOrDispose { }
    }
    return gate
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}
