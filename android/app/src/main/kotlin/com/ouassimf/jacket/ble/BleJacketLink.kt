package com.ouassimf.jacket.ble

import android.bluetooth.le.ScanSettings
import android.util.Log
import com.juul.kable.AndroidPeripheral
import com.juul.kable.Characteristic
import com.juul.kable.ObsoleteKableApi
import com.juul.kable.Peripheral
import com.juul.kable.Scanner
import com.juul.kable.State
import com.juul.kable.UnmetRequirementException
import com.juul.kable.WriteType
import com.juul.kable.characteristicOf
import com.ouassimf.jacket.link.JacketLink
import com.ouassimf.jacket.link.LinkFailure
import com.ouassimf.jacket.link.LinkStatus
import com.ouassimf.jacket.link.ReconnectPolicy
import com.ouassimf.jacket.protocol.Protocol
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.IOException
import kotlin.uuid.ExperimentalUuidApi
import kotlin.uuid.Uuid

/**
 * The jacket over Bluetooth LE, through Kable.
 *
 * One coroutine owns the whole life of the link: scan for the service, connect, negotiate the MTU, subscribe to
 * `state`, read it, then sit until the connection drops and start over with a growing pause. [disconnect]
 * cancels that coroutine, which is the only way anything here stops, so there is nothing to leak.
 *
 * The app must hold BLUETOOTH_SCAN and BLUETOOTH_CONNECT before [connect]; without them, or with Bluetooth off,
 * the link ends in [LinkStatus.Failed] and waits for the next [connect].
 *
 * Call it from [parentScope]'s single-threaded dispatcher (the main thread); Kable's suspending calls are main-safe.
 */
@OptIn(ExperimentalUuidApi::class)
class BleJacketLink(parentScope: CoroutineScope) : JacketLink {

    private val scope = CoroutineScope(parentScope.coroutineContext + SupervisorJob(parentScope.coroutineContext[Job]))

    private val _status = MutableStateFlow<LinkStatus>(LinkStatus.Disconnected)
    override val status: StateFlow<LinkStatus> = _status.asStateFlow()

    private val _frames = MutableSharedFlow<ByteArray>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    override val frames: SharedFlow<ByteArray> = _frames.asSharedFlow()

    private var job: Job? = null

    /** The peripheral while it is connected and set up, i.e. exactly while [status] is Connected. */
    private var connected: Peripheral? = null
    private var highPriority = false

    override fun connect() {
        val previous = job
        if (previous?.isActive == true) return
        // A link that was just told to disconnect may still be closing its GATT client: let it finish first.
        job = scope.launch {
            previous?.join()
            run()
        }
    }

    override fun disconnect() {
        job?.cancel()
        _status.value = LinkStatus.Disconnected
    }

    override suspend fun send(frame: ByteArray, withResponse: Boolean) =
        write(COMMAND, frame, if (withResponse) WriteType.WithResponse else WriteType.WithoutResponse)

    override suspend fun sendLive(frame: ByteArray) = write(LIVE, frame, WriteType.WithoutResponse)

    override fun setHighPriority(enabled: Boolean) {
        highPriority = enabled
        applyPriority()
    }

    override fun close() {
        _status.value = LinkStatus.Disconnected
        scope.cancel()
    }

    private suspend fun write(characteristic: Characteristic, frame: ByteArray, type: WriteType) {
        val peripheral = connected ?: throw IOException("jacket is not connected")
        try {
            peripheral.write(characteristic, frame, type)
        } catch (e: CancellationException) {
            throw e
        } catch (e: IOException) {
            throw e
        } catch (e: Exception) {
            throw IOException(e) // e.g. the characteristic is missing: to the caller it is one more failed write
        }
    }

    private fun applyPriority() {
        val peripheral = connected as? AndroidPeripheral ?: return
        val priority = if (highPriority) AndroidPeripheral.Priority.High else AndroidPeripheral.Priority.Balanced
        try {
            peripheral.requestConnectionPriority(priority)
        } catch (e: Exception) {
            Log.w(TAG, "connection priority request failed", e)
        }
    }

    // ---- the connection loop ----------------------------------------------------------------------------------

    private suspend fun run() {
        var peripheral: Peripheral? = null
        var failures = 0
        try {
            while (true) {
                try {
                    val target = peripheral ?: discover().also { peripheral = it }
                    _status.value = LinkStatus.Connecting
                    session(target, onReady = { failures = 0 })
                } catch (e: CancellationException) {
                    throw e
                } catch (e: MtuTooLowException) {
                    return fail(LinkFailure.MtuTooLow, e)
                } catch (e: UnmetRequirementException) {
                    return fail(LinkFailure.BluetoothOff, e)
                } catch (e: SecurityException) {
                    return fail(LinkFailure.PermissionMissing, e)
                } catch (e: Exception) {
                    Log.w(TAG, "connection attempt failed", e)
                }
                // Dropped, or never got there. Try the same device again a few times: that needs no scan and is
                // the fastest way back. If it keeps failing, forget it and look for the jacket from scratch.
                failures++
                if (failures >= ReconnectPolicy.RESCAN_AFTER_FAILURES) {
                    peripheral?.close()
                    peripheral = null
                }
                _status.value = LinkStatus.Connecting
                delay(ReconnectPolicy.backoff(failures))
            }
        } finally {
            peripheral?.let { release(it) }
        }
    }

    private suspend fun fail(reason: LinkFailure, cause: Exception) {
        Log.w(TAG, "giving up: $reason", cause)
        if (currentCoroutineContext().isActive) _status.value = LinkStatus.Failed(reason)
    }

    /**
     * Scans until the jacket's service shows up: eagerly at first, then at low power for as long as it takes.
     * The service UUID is all that identifies the jacket: its name travels in the scan response, which may not
     * arrive, so nothing here asks for it.
     */
    private suspend fun discover(): Peripheral {
        _status.value = LinkStatus.Scanning
        val advertisement = withTimeoutOrNull(ReconnectPolicy.FAST_SCAN) { scanner(ScanSettings.SCAN_MODE_LOW_LATENCY).advertisements.first() }
            ?: scanner(ScanSettings.SCAN_MODE_LOW_POWER).advertisements.first()
        return Peripheral(advertisement) {
            // Before any subscription, as the document orders it. The result is checked once connected.
            onServicesDiscovered { requestMtu(Protocol.REQUESTED_MTU) }
        }
    }

    @OptIn(ObsoleteKableApi::class)
    private fun scanner(mode: Int) = Scanner {
        filters { match { services = listOf(SERVICE) } }
        scanSettings = ScanSettings.Builder().setScanMode(mode).build()
    }

    /** One connection, from connect to drop. Returns when the jacket goes away; throws when it cannot be set up. */
    private suspend fun session(peripheral: Peripheral, onReady: () -> Unit) {
        try {
            peripheral.connect()
            val mtu = (peripheral as AndroidPeripheral).mtu.value ?: DEFAULT_ATT_MTU
            if (mtu < Protocol.MIN_MTU) throw MtuTooLowException(mtu)

            coroutineScope {
                val observation = launch {
                    peripheral
                        .observe(STATE) {
                            // Subscribed. The read gives the latest STATE, and only then is the link usable.
                            _frames.emit(peripheral.read(STATE))
                            connected = peripheral
                            applyPriority()
                            onReady()
                            _status.value = LinkStatus.Connected
                        }
                        .collect { _frames.emit(it) }
                }
                peripheral.state.first { it is State.Disconnected || it is State.Disconnecting }
                observation.cancel()
            }
        } finally {
            connected = null
            withContext(NonCancellable) {
                try {
                    peripheral.disconnect()
                } catch (e: Exception) {
                    Log.w(TAG, "disconnect failed", e)
                }
            }
        }
    }

    private fun release(peripheral: Peripheral) {
        try {
            peripheral.close()
        } catch (e: Exception) {
            Log.w(TAG, "close failed", e)
        }
    }

    private class MtuTooLowException(mtu: Int) : IOException("negotiated MTU $mtu is below ${Protocol.MIN_MTU}")

    private companion object {
        const val TAG = "JacketBle"
        const val DEFAULT_ATT_MTU = 23

        val SERVICE = Uuid.parse(Protocol.SERVICE_UUID)
        val COMMAND = characteristicOf(SERVICE, Uuid.parse(Protocol.COMMAND_UUID))
        val STATE = characteristicOf(SERVICE, Uuid.parse(Protocol.STATE_UUID))
        val LIVE = characteristicOf(SERVICE, Uuid.parse(Protocol.LIVE_UUID))
    }
}
