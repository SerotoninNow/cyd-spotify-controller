plugins {
    id("com.android.application")
}

android {
    namespace = "com.cyd.nowplaying"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.cyd.nowplaying"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
