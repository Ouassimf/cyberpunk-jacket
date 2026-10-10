package com.ouassimf.jacket

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.ouassimf.jacket.ui.JacketApp

class MainActivity : ComponentActivity() {

    private val app get() = application as JacketApplication

    override fun onCreate(savedInstanceState: Bundle?) {
        // The app is dark whatever the system theme says, so the bars always get light icons.
        enableEdgeToEdge(SystemBarStyle.dark(Color.TRANSPARENT), SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        setContent { JacketApp(app.repository, app.live) }
    }

    override fun onStart() {
        super.onStart()
        app.onUiStarted()
    }

    override fun onStop() {
        super.onStop()
        if (!isChangingConfigurations) app.onUiStopped()
    }
}
