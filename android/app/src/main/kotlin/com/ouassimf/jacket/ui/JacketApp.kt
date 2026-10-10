package com.ouassimf.jacket.ui

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsTopHeight
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.live.LiveController
import com.ouassimf.jacket.service.LiveService

/**
 * The screens reachable from the bottom bar. To add one: an entry here and a branch in [JacketApp]. A handful
 * of tabs with no back stack does not need a navigation library.
 */
private enum class Screen(val label: String, val icon: ImageVector) {
    Home("Home", AppIcons.Home),
    Zones("Zones", AppIcons.Zones),
    Live("Live", AppIcons.Live),
    Presets("Presets", AppIcons.Presets),
    Settings("Settings", AppIcons.Settings),
}

/**
 * @param startLive starts live mode with whatever must run around it. In the app that is the foreground service,
 *   which keeps it going in the background; tests start the controller alone.
 */
@Composable
fun JacketApp(repository: JacketRepository, live: LiveController, startLive: (Context) -> Unit = LiveService::start) {
    JacketTheme {
        val demo by repository.demoMode.collectAsStateWithLifecycle()
        val gate = rememberBluetoothGate(askOnFirstUse = !demo)

        // Connect whenever the app comes to the front and nothing is in the way; again when demo mode flips or
        // Bluetooth becomes usable. The repository ignores it if it is already connected. Letting go in the
        // background is the application's business, see JacketApplication.onUiStopped.
        LifecycleStartEffect(demo, gate.ready) {
            if (demo || gate.ready) repository.connect()
            onStopOrDispose { }
        }

        // A command the jacket refused, whichever screen sent it.
        val snackbar = remember { SnackbarHostState() }
        val error by repository.error.collectAsStateWithLifecycle()
        LaunchedEffect(error) {
            val refused = error ?: return@LaunchedEffect
            snackbar.showSnackbar(errorMessage(refused), withDismissAction = true, duration = SnackbarDuration.Long)
            repository.dismissError()
        }

        var screen by rememberSaveable { mutableStateOf(Screen.Home) }
        // Each tab keeps what it remembers (a half-edited zone layout, which zone Home edits, how far it was
        // scrolled) while another tab is in front.
        val tabs = rememberSaveableStateHolder()

        Scaffold(
            // The system draws the clock and icons over the app: this keeps scrolled content from showing through them.
            topBar = { Spacer(Modifier.fillMaxWidth().windowInsetsTopHeight(WindowInsets.statusBars).background(Neon.Background)) },
            snackbarHost = {
                SnackbarHost(snackbar) {
                    Snackbar(it, containerColor = Neon.PanelRaised, contentColor = Neon.Text, dismissActionContentColor = Neon.TextDim)
                }
            },
            bottomBar = {
                NavigationBar(containerColor = Neon.Panel) {
                    for (entry in Screen.entries) {
                        NavigationBarItem(
                            selected = entry == screen,
                            onClick = { screen = entry },
                            icon = { Icon(entry.icon, contentDescription = null) },
                            // Five tabs share the width: without the labels' usual letter spacing the longest still fits.
                            label = { Text(entry.label, maxLines = 1, softWrap = false, letterSpacing = 0.sp) },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = Neon.Background,
                                selectedTextColor = Neon.Cyan,
                                indicatorColor = Neon.Cyan,
                                unselectedIconColor = Neon.TextDim,
                                unselectedTextColor = Neon.TextDim,
                            ),
                        )
                    }
                }
            },
        ) { insets ->
            val direction = LocalLayoutDirection.current
            val padding = PaddingValues(
                start = insets.calculateStartPadding(direction) + 16.dp,
                top = insets.calculateTopPadding() + 16.dp,
                end = insets.calculateEndPadding(direction) + 16.dp,
                bottom = insets.calculateBottomPadding() + 24.dp,
            )
            tabs.SaveableStateProvider(screen) {
                when (screen) {
                    Screen.Home -> HomeScreen(repository, gate, padding)
                    Screen.Zones -> ZonesScreen(repository, padding)
                    Screen.Live -> LiveScreen(repository, live, startLive, padding)
                    Screen.Presets -> PresetsScreen(repository, padding)
                    Screen.Settings -> SettingsScreen(repository, padding)
                }
            }
        }
    }
}
