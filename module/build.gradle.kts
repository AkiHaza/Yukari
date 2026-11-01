plugins {
    id("com.android.application")
}

android {
    namespace = "com.yukari.module"
    compileSdk = providers.gradleProperty("yukari.compileSdk").get().toInt()
    buildToolsVersion = providers.gradleProperty("yukari.buildToolsVersion").get()
    ndkVersion = providers.gradleProperty("yukari.ndkVersion").get()

    defaultConfig {
        applicationId = "com.yukari.module.stub"
        minSdk = providers.gradleProperty("yukari.minSdk").get().toInt()
        targetSdk = providers.gradleProperty("yukari.targetSdk").get().toInt()
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-Wall", "-Wextra", "-fvisibility=hidden")
                arguments += listOf("-DANDROID_STL=c++_static")
                abiFilters += listOf("arm64-v8a")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = providers.gradleProperty("yukari.cmakeVersion").get()
        }
    }
}
