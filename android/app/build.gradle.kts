plugins {
    id("com.android.application")
}

android {
    namespace = "__PACKAGE_NAME__"
    compileSdk = 35

    defaultConfig {
        applicationId = "__PACKAGE_NAME__"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments(
                    "-DGRAMARYE_TARGET=android",
                    "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja"
                )
                abiFilters("arm64-v8a", "armeabi-v7a", "x86_64")
            }
        }
    }

    // Points to the game's CMakeLists.txt one level up from android/.
    // Path is relative to this app/ directory: app/ -> ../  -> android/ -> ../../ -> game root.
    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.27.0+"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    sourceSets {
        getByName("main") {
            // ../../assets = <game root>/assets, shared with desktop/web builds.
            assets.srcDirs("src/main/assets", "../../assets")
        }
    }
}
