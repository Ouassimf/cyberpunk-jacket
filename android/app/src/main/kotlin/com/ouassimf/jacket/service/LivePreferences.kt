package com.ouassimf.jacket.service

import android.content.SharedPreferences
import com.ouassimf.jacket.live.LiveSettings

/** The Live screen's settings in the app's preferences file: four values, so nothing heavier than that. */
internal object LivePreferences {
    private const val SOUND = "live_sound"
    private const val MOTION = "live_motion"
    private const val SENSITIVITY = "live_sensitivity"
    private const val LATENCY_OFFSET = "live_latency_offset_ms"

    fun load(preferences: SharedPreferences): LiveSettings {
        val defaults = LiveSettings()
        return LiveSettings(
            sound = preferences.getBoolean(SOUND, defaults.sound),
            motion = preferences.getBoolean(MOTION, defaults.motion),
            sensitivity = preferences.getInt(SENSITIVITY, defaults.sensitivity),
            latencyOffsetMs = preferences.getInt(LATENCY_OFFSET, defaults.latencyOffsetMs),
        ).sanitized()
    }

    fun save(preferences: SharedPreferences, settings: LiveSettings) {
        preferences.edit()
            .putBoolean(SOUND, settings.sound)
            .putBoolean(MOTION, settings.motion)
            .putInt(SENSITIVITY, settings.sensitivity)
            .putInt(LATENCY_OFFSET, settings.latencyOffsetMs)
            .apply()
    }
}
