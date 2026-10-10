package com.ouassimf.jacket.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.BuildConfig
import com.ouassimf.jacket.data.Change
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.protocol.DeviceInfo
import com.ouassimf.jacket.protocol.Protocol

/** The three power budgets offered; the jacket accepts anything from 400 to 2000 mA. */
private enum class Budget(val label: String, val milliamps: Int) {
    Eco("Eco", 500),
    Normal("Normal", 900),
    Max("Max", 2000),
}

@Composable
fun SettingsScreen(repository: JacketRepository, contentPadding: PaddingValues) {
    val connection by repository.connection.collectAsStateWithLifecycle()
    val state by repository.state.collectAsStateWithLifecycle()
    val info by repository.info.collectAsStateWithLifecycle()
    val demo by repository.demoMode.collectAsStateWithLifecycle()
    val online = connection == ConnectionStatus.Ready || connection == ConnectionStatus.Syncing
    val current = state

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("SETTINGS", style = MaterialTheme.typography.headlineSmall.copy(brush = Neon.Sweep))

        SectionLabel("Power budget")
        Panel {
            Text(
                "The most current the whole jacket may draw at the battery. The firmware dims the LEDs to stay under it.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )
            Row(Modifier.selectableGroup(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (budget in Budget.entries) {
                    BudgetOption(
                        budget = budget,
                        selected = current?.budgetMa == budget.milliamps,
                        enabled = online && current != null,
                        onSelect = { repository.commit(Change.Budget(budget.milliamps)) },
                        modifier = Modifier.weight(1f),
                    )
                }
            }
            InfoRow("Jacket is set to", current?.let { "${it.budgetMa} mA" } ?: "unknown")
            Text(
                "About ${Protocol.IDLE_DRAW_MA} mA of any budget is the jacket's idle draw and makes no light.",
                style = MaterialTheme.typography.bodySmall,
                color = Neon.TextDim,
            )
        }

        SectionLabel("Developer")
        Panel {
            SwitchRow(
                title = "Dev mode",
                detail = "Turns on WiFi, over-the-air updates and the log stream on the jacket. Uses more power.",
                checked = current?.devMode == true,
                enabled = online && current != null,
                onChange = { repository.commit(Change.DevMode(it)) },
            )
            if (current?.devMode == true) {
                InfoRow("Jacket WiFi", if (current.wifiConnected) "connected" else "not connected")
            }
        }

        SectionLabel("Firmware")
        Panel { FirmwareInfo(info) }

        SectionLabel("App")
        Panel {
            SwitchRow(
                title = "Demo mode",
                detail = "Drives a simulated jacket inside the app instead of the real one. No Bluetooth needed.",
                checked = demo,
                enabled = true,
                onChange = repository::setDemoMode,
            )
            InfoRow("App version", BuildConfig.VERSION_NAME)
            InfoRow("Protocol", "v${Protocol.VERSION}")
        }
    }
}

@Composable
private fun BudgetOption(budget: Budget, selected: Boolean, enabled: Boolean, onSelect: () -> Unit, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(8.dp)
    Column(
        modifier
            .alpha(if (enabled) 1f else 0.4f)
            .clip(shape)
            .background(if (selected) Neon.Cyan else Neon.PanelRaised)
            .border(1.dp, if (selected) Neon.Cyan else Neon.Line, shape)
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onSelect)
            .padding(vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        val content = if (selected) Neon.Background else Neon.Text
        Text(budget.label.uppercase(), style = MaterialTheme.typography.labelLarge, color = content)
        Text("${budget.milliamps} mA", style = MaterialTheme.typography.labelMedium, color = content.copy(alpha = 0.8f))
    }
}

@Composable
private fun FirmwareInfo(info: DeviceInfo?) {
    if (info == null) {
        Text("Connect to the jacket to see what it runs.", style = MaterialTheme.typography.bodyMedium, color = Neon.TextDim)
        return
    }
    InfoRow("Version", info.version)
    InfoRow("Build", info.build.ifEmpty { "-" })
    InfoRow("Protocol", "v${info.proto}")
    InfoRow("LED strips", info.leds.joinToString(" + ") { "$it" }.ifEmpty { "none" } + " LEDs")
    InfoRow("Segments", "up to ${info.maxSegments}")
    InfoRow("Preset slots", "${info.presetSlots}")
    InfoRow("Effects", "${info.effectCount}")
    InfoRow("Palettes", "${info.paletteCount}")
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = Neon.TextDim)
        Text(value, style = MaterialTheme.typography.labelLarge, color = Neon.Text)
    }
}
