plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// The golden frames live next to PROTOCOL.md and are shared with the firmware tests; never copy them.
val protocolVectors = rootProject.layout.projectDirectory.file("../protocol/vectors/v1.json")

android {
    namespace = "com.ouassimf.jacket"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.ouassimf.jacket"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources.excludes += setOf("META-INF/{AL2.0,LGPL2.1}", "META-INF/*.kotlin_module", "DebugProbesKt.bin")
    }

    testOptions {
        unitTests.isIncludeAndroidResources = true // for the Robolectric screen tests
        unitTests.all { test ->
            test.inputs.file(protocolVectors).withPathSensitivity(PathSensitivity.NONE)
            test.systemProperty("jacket.vectors", protocolVectors.asFile.absolutePath)
            // Optional: -Pjacket.screenshots=<dir> makes the screen tests save a PNG of each screen there.
            providers.gradleProperty("jacket.screenshots").orNull?.let { test.systemProperty("jacket.screenshots", it) }
            // Robolectric loads its native graphics library; say so, as recent JDKs ask.
            test.jvmArgs("--enable-native-access=ALL-UNNAMED")
        }
    }
}

androidComponents {
    onVariants(selector().withBuildType("debug")) { variant ->
        // Debug is not shrunk, and uncompressed dex made it a 30 MB APK. Compressed it is a third of that,
        // which matters when every build is pushed to the phone over wireless adb. Release keeps the default.
        variant.packaging.dex.useLegacyPackaging.set(true)
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

dependencies {
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.runtime.compose)
    implementation(libs.coroutines.android)
    implementation(libs.kable.core)

    testImplementation(libs.junit)
    testImplementation(libs.coroutines.test)
    testImplementation(libs.serialization.json)
    testImplementation(libs.robolectric)
    testImplementation(libs.compose.ui.test.junit4)
}
