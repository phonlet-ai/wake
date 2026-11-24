pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement { repositories { google(); mavenCentral() } }
rootProject.name = "wake-android-demo"
include(":app")
include(":wake")  // shared engine+JNI library from the SDK checkout
project(":wake").projectDir = file("../../sdk/android/wake")
