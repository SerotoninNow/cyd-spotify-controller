import java.util.Properties

plugins {
    id("com.android.application")
}

// The release signing key lives outside the repository. Locally that's
// ~/.config/cyd-now-playing/keystore.properties; the GitHub workflow writes the same file from
// its secrets. Without it, release builds come out unsigned (debug builds don't need it).
val keystoreProperties = Properties().apply {
    val path = System.getenv("CYD_KEYSTORE_PROPERTIES")
        ?: "${System.getProperty("user.home")}/.config/cyd-now-playing/keystore.properties"
    val f = file(path)
    if (f.exists()) f.inputStream().use { load(it) }
}

android {
    namespace = "com.cyd.nowplaying"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.cyd.nowplaying"
        minSdk = 26
        targetSdk = 36
        // The release workflow passes these from the tag (app-v1.2 -> 1.2) and its run number.
        versionCode = (findProperty("versionCode") as String?)?.toInt() ?: 1
        versionName = (findProperty("versionName") as String?) ?: "1.0"
    }

    signingConfigs {
        if (keystoreProperties.containsKey("storeFile")) {
            create("release") {
                storeFile = file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
