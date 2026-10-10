package com.ouassimf.jacket.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.ouassimf.jacket.data.ConnectionStatus
import com.ouassimf.jacket.data.JacketRepository
import com.ouassimf.jacket.protocol.ErrorCode
import com.ouassimf.jacket.protocol.ErrorReply
import com.ouassimf.jacket.protocol.Opcode
import com.ouassimf.jacket.protocol.Protocol
import com.ouassimf.jacket.protocol.truncateUtf8
import com.ouassimf.jacket.protocol.utf8Size

/**
 * The jacket's preset slots: what is in each, which one the current scene comes from, and save, load, delete.
 * Nothing here pretends: the list and the active mark change when the jacket says so.
 */
@Composable
fun PresetsScreen(repository: JacketRepository, contentPadding: PaddingValues) {
    val connection by repository.connection.collectAsStateWithLifecycle()
    val state by repository.state.collectAsStateWithLifecycle()
    val presets by repository.presets.collectAsStateWithLifecycle()
    val info by repository.info.collectAsStateWithLifecycle()
    val online = connection == ConnectionStatus.Ready || connection == ConnectionStatus.Syncing
    val current = state
    val listed = presets

    // The slot being named, and the slot whose deletion is being confirmed. Saved, so a rotation keeps them.
    var naming by rememberSaveable { mutableStateOf<Int?>(null) }
    var deleting by rememberSaveable { mutableStateOf<Int?>(null) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(contentPadding),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("PRESETS", style = MaterialTheme.typography.headlineSmall.copy(brush = Neon.Sweep))
        Text(
            "A preset keeps the brightness, the zones and what each zone plays. Power is not part of it.",
            style = MaterialTheme.typography.bodySmall,
            color = Neon.TextDim,
        )

        when {
            current == null -> Text(
                "Connect to the jacket to see its presets.",
                modifier = Modifier.padding(vertical = 24.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = Neon.TextDim,
            )
            listed == null -> Panel {
                Text(
                    if (connection == ConnectionStatus.Syncing) "Reading the presets…" else "The jacket has not listed its presets.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = Neon.TextDim,
                )
                if (connection == ConnectionStatus.Ready) {
                    NeonOutlinedButton(onClick = repository::refreshPresets) { Text("Ask again") }
                }
            }
            else -> for (slot in 0 until (info?.presetSlots ?: Protocol.PRESET_SLOTS)) {
                PresetRow(
                    slot = slot,
                    name = listed.firstOrNull { it.slot == slot }?.name,
                    used = listed.any { it.slot == slot },
                    active = current.preset == slot,
                    naming = naming == slot,
                    enabled = online,
                    onLoad = { repository.loadPreset(slot) },
                    onName = { naming = slot },
                    onSave = { name ->
                        repository.savePreset(slot, name)
                        naming = null
                    },
                    onCancel = { naming = null },
                    onDelete = { deleting = slot },
                )
            }
        }
    }

    deleting?.let { slot ->
        val name = listed?.firstOrNull { it.slot == slot }?.name
        AlertDialog(
            onDismissRequest = { deleting = null },
            title = { Text("Delete ${quoted(name, slot)}?", style = MaterialTheme.typography.titleMedium) },
            text = { Text("Slot ${slot + 1} becomes empty. The lights do not change.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        repository.deletePreset(slot)
                        deleting = null
                    },
                    colors = ButtonDefaults.textButtonColors(contentColor = Neon.Danger),
                ) { Text("Delete") }
            },
            dismissButton = { TextButton(onClick = { deleting = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun PresetRow(
    slot: Int,
    name: String?,
    used: Boolean,
    active: Boolean,
    naming: Boolean,
    enabled: Boolean,
    onLoad: () -> Unit,
    onName: () -> Unit,
    onSave: (String) -> Unit,
    onCancel: () -> Unit,
    onDelete: () -> Unit,
) {
    val label = "Slot ${slot + 1}"
    Column(
        Modifier
            .fillMaxWidth()
            .background(if (active) Neon.PanelRaised else Neon.Panel, PanelShape)
            .border(if (active) 2.dp else 1.dp, if (active) Neon.Magenta else Neon.Line, PanelShape)
            .padding(horizontal = 14.dp, vertical = 10.dp)
            .semantics { contentDescription = label },
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Box(
                Modifier.size(32.dp).border(1.dp, if (used) Neon.Cyan else Neon.Line, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Text("${slot + 1}", style = MaterialTheme.typography.labelLarge, color = if (used) Neon.Cyan else Neon.TextDim)
            }
            Column(Modifier.weight(1f)) {
                Text(
                    text = if (used) name.orEmpty().ifBlank { "Unnamed" } else "Empty",
                    style = MaterialTheme.typography.titleSmall,
                    color = if (used) Neon.Text else Neon.TextDim,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (active) Text("ACTIVE", style = MaterialTheme.typography.labelSmall, color = Neon.Magenta)
            }
            if (!used && !naming) NeonOutlinedButton(onClick = onName, enabled = enabled) { Text("Save here") }
        }
        when {
            naming -> NameEditor(replaces = name.takeIf { used }, slot = slot, enabled = enabled, onSave = onSave, onCancel = onCancel)
            used -> Row(Modifier.fillMaxWidth().alpha(if (enabled) 1f else 0.4f), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onDelete, enabled = enabled, colors = ButtonDefaults.textButtonColors(contentColor = Neon.Danger)) {
                    Text("Delete")
                }
                TextButton(onClick = onName, enabled = enabled) { Text("Overwrite") }
                Button(onClick = onLoad, enabled = enabled) { Text("Load") }
            }
        }
    }
}

/**
 * Names the preset, in the row itself. The protocol allows 16 bytes of UTF-8, which is fewer than 16 characters
 * once accents or emoji come in, so the field cuts by bytes and shows the count.
 */
@Composable
private fun NameEditor(replaces: String?, slot: Int, enabled: Boolean, onSave: (String) -> Unit, onCancel: () -> Unit) {
    var name by rememberSaveable(slot) { mutableStateOf(replaces.orEmpty().truncateUtf8(Protocol.MAX_PRESET_NAME_BYTES)) }
    val canSave = enabled && name.isNotBlank()
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }

    OutlinedTextField(
        value = name,
        onValueChange = { name = it.replace("\n", "").truncateUtf8(Protocol.MAX_PRESET_NAME_BYTES) },
        singleLine = true,
        label = { Text("Name") },
        supportingText = {
            val what = if (replaces != null) "Replaces ${quoted(replaces, slot)} with" else "Stores"
            Text("${name.utf8Size} of ${Protocol.MAX_PRESET_NAME_BYTES} bytes. $what what the jacket shows now.")
        },
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { if (canSave) onSave(name.trim()) }),
        modifier = Modifier.fillMaxWidth().focusRequester(focus),
    )
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        TextButton(onClick = onCancel) { Text("Cancel") }
        Button(onClick = { onSave(name.trim()) }, enabled = canSave) { Text("Save") }
    }
}

private fun quoted(name: String?, slot: Int) = if (name.isNullOrBlank()) "slot ${slot + 1}" else "“$name”"

/** What to tell the user when the jacket answers a command with ERROR. */
fun errorMessage(error: ErrorReply): String = when (error.code) {
    ErrorCode.NOT_FOUND -> "That preset slot is empty."
    ErrorCode.STORAGE_FAILURE ->
        if (error.opcode == Opcode.PRESET_DELETE) "The jacket could not delete the preset: its storage failed." else "The jacket could not save the preset: its storage failed."
    ErrorCode.OUT_OF_RANGE -> "The jacket refused a value it does not accept."
    ErrorCode.TOO_SHORT, ErrorCode.UNKNOWN_OPCODE -> "The jacket did not understand the app. One of them may be out of date."
    else -> "The jacket reported error ${error.code}."
}
