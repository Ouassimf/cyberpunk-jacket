package com.ouassimf.jacket.ui

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.LifecycleResumeEffect

/** What stands between the app and a Bluetooth connection, most blocking first. */
enum class BluetoothProblem {
    /** The device has no Bluetooth radio (an emulator, typically). Only demo mode works. */
    Unavailable,

    /** Not granted yet; the system dialog can still be shown. */
    PermissionNeeded,

    /** Denied for good: only the app's page in system settings can change it. */
    PermissionBlocked,

    BluetoothOff,
}

/** The two things Android wants before any Bluetooth LE call: the runtime permissions, and the radio on. */
@Stable
class BluetoothGate internal constructor(private val context: Context) {

    var problem: BluetoothProblem? by mutableStateOf(null)
        private set

    val ready: Boolean get() = problem == null

    private var blocked = false
    internal var askPermissions: () -> Unit = {}
    internal var askToEnable: () -> Unit = {}

    init {
        refresh()
    }

    /** Does whatever resolves [problem]: asks for the permissions, opens settings, or asks to switch Bluetooth on. */
    fun resolve() {
        when (problem) {
            BluetoothProblem.PermissionNeeded -> askPermissions()
            BluetoothProblem.PermissionBlocked -> context.startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            )
            BluetoothProblem.BluetoothOff -> askToEnable()
            BluetoothProblem.Unavailable, null -> Unit
        }
    }

    internal fun onPermissionResult(granted: Map<String, Boolean>) {
        // Once the system stops offering the dialog, a denial comes straight back, with no rationale to show.
        val activity = context.findActivity()
        blocked = granted.values.any { !it } && activity != null && PERMISSIONS.none(activity::shouldShowRequestPermissionRationale)
        refresh()
    }

    internal fun refresh() {
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
        val permitted = PERMISSIONS.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
        if (permitted) blocked = false
        problem = when {
            adapter == null -> BluetoothProblem.Unavailable
            !permitted -> if (blocked) BluetoothProblem.PermissionBlocked else BluetoothProblem.PermissionNeeded
            !adapter.isEnabled -> BluetoothProblem.BluetoothOff
            else -> null
        }
    }

    internal companion object {
        val PERMISSIONS = arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    }
}

/**
 * A [BluetoothGate] that follows permissions and the radio live. With [askOnFirstUse] the permission dialog
 * comes up once by itself; after that only [BluetoothGate.resolve], i.e. a button, shows it.
 */
@Composable
fun rememberBluetoothGate(askOnFirstUse: Boolean): BluetoothGate {
    val context = LocalContext.current
    val gate = remember(context) { BluetoothGate(context) }

    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions(), gate::onPermissionResult)
    val enable = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { gate.refresh() }
    gate.askPermissions = { permissions.launch(BluetoothGate.PERMISSIONS) }
    gate.askToEnable = { enable.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }

    // The radio can be switched from the quick settings while the app is in front.
    DisposableEffect(gate) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) = gate.refresh()
        }
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(receiver, filter)
        }
        onDispose { context.unregisterReceiver(receiver) }
    }

    // Permissions can change in system settings while the app is paused.
    LifecycleResumeEffect(gate) {
        gate.refresh()
        onPauseOrDispose { }
    }

    var asked by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(askOnFirstUse) {
        if (askOnFirstUse && !asked && gate.problem == BluetoothProblem.PermissionNeeded) {
            asked = true
            permissions.launch(BluetoothGate.PERMISSIONS)
        }
    }
    return gate
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}
