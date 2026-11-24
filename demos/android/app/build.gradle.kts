plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}
android {
    namespace = "ai.phonlet.wake.demos"
    compileSdk = 35
    defaultConfig {
        applicationId = "ai.phonlet.wake.demos"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    }
    buildTypes { release { isMinifyEnabled = false } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    // Ship the production model and enrollment array as demo assets.
    sourceSets.getByName("main").assets.srcDir(layout.buildDirectory.dir("generated/assets"))
}
dependencies {
    implementation(project(":wake"))
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core-ktx:1.15.0")
}
// Copy pack artifacts into the demo assets at build time (never committed).
val packAssets = tasks.register<Copy>("packAssets") {
    from("../../../resources/models/en-1") { include("model.pwake") }
    from("../../../resources/keywords") { include("en.json"); rename { "keywords.json" } }
    into(layout.buildDirectory.dir("generated/assets"))
}
tasks.preBuild { dependsOn(packAssets) }
