plugins {
    // Kotlin support is built into AGP 9 — no separate Kotlin plugin needed.
    alias(libs.plugins.android.library)
}

android {
    namespace = "com.uxspace.glasses"
    compileSdk = 36
    ndkVersion = "30.0.14904198"

    defaultConfig {
        minSdk = 26

        // The VITURE SDK ships arm64 binaries; this phone is arm64.
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // Builds libglasses_bridge.so — the JNI bridge to the native VITURE SDK.
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

// ---- The open viture-v2 driver ---------------------------------------------
//
// Upstream links VITURE's proprietary libglasses.so here, which every user had
// to request and vendor by hand. This fork builds the driver from source as
// part of the normal build: the Rust crate lives in a submodule and produces
// libviture_v2.so, which the JNI bridge links against.
//
// Everything is resolved at configuration time into plain values, so the task
// stays compatible with Gradle's configuration cache, and no AGP internals are
// touched — those move between major versions.

val ndkPin = "30.0.14904198"
val apiPin = 26
val rustTarget = "aarch64-linux-android"
val rustAbi = "arm64-v8a"
val rustDir = layout.projectDirectory.dir("viture-v2")

val sdkDir: String = System.getenv("ANDROID_HOME")
    ?: System.getenv("ANDROID_SDK_ROOT")
    ?: rootProject.file("local.properties").takeIf { it.exists() }
        ?.readLines()
        ?.firstOrNull { line -> line.startsWith("sdk.dir=") }
        ?.substringAfter("=")
    ?: ""

val hostTag: String =
    if (System.getProperty("os.name").startsWith("Mac")) "darwin-x86_64" else "linux-x86_64"
val ndkLinker: File =
    File("$sdkDir/ndk/$ndkPin/toolchains/llvm/prebuilt/$hostTag/bin/aarch64-linux-android$apiPin-clang")
val cargoBin: String =
    System.getenv("CARGO") ?: "${System.getProperty("user.home")}/.cargo/bin/cargo"
val cargoManifest: File = rustDir.file("Cargo.toml").asFile
val rustArtifact: File = rustDir.file("target/$rustTarget/release/libviture_v2.so").asFile

// Checked while configuring rather than in a doFirst block: a script lambda
// inside the task would capture the build script and break the configuration
// cache, and failing early gives a clearer message anyway.
check(cargoManifest.exists()) {
    "viture-v2 submodule is missing — run: git submodule update --init --recursive"
}
check(ndkLinker.exists()) { "NDK linker not found at $ndkLinker" }

val buildRustDriver = tasks.register<Exec>("buildRustDriver") {
    group = "build"
    description = "Builds libviture_v2.so, the open replacement for the VITURE SDK."

    workingDir = rustDir.asFile
    inputs.dir(rustDir.dir("src"))
    inputs.file(rustDir.file("Cargo.toml"))
    outputs.file(rustArtifact)

    environment("CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER", ndkLinker.absolutePath)
    // render: the panorama geometry and camera maths the 360° player needs.
    // sim:    a pair of glasses with nothing on the other end of the cable.
    //
    // The simulation ships rather than being test-only, and that is deliberate.
    // The glasses are one cable and one battery, and everything above them —
    // the whole renderer, the workspace, the media library — should not stop
    // being workable when either runs out. It also reaches the cases hardware
    // cannot be asked for: a head turning at exactly 45 degrees a second, a
    // pose stream that dies mid-session, a panel that refuses a mode.
    //
    // It costs a few kilobytes and no dependencies, and it is opt-in at
    // runtime: nothing selects it unless asked.
    commandLine(
        cargoBin, "build", "--release", "--target", rustTarget,
        "--features", "render,sim",
    )
}

dependencies {
    // JUnit only, and already in the version catalogue — this module's tests
    // read the driver's own source to check the numbers that cross the JNI
    // boundary, so they need nothing from Android.
    testImplementation(libs.junit)
}

val installRustDriver = tasks.register<Copy>("installRustDriver") {
    group = "build"
    description = "Places libviture_v2.so where the JNI bridge and the APK expect it."
    dependsOn(buildRustDriver)
    from(rustArtifact)
    into(layout.projectDirectory.dir("src/main/jniLibs/$rustAbi"))
}

tasks.matching { it.name == "preBuild" }.configureEach { dependsOn(installRustDriver) }
tasks.matching { it.name.startsWith("configureCMake") }.configureEach {
    dependsOn(installRustDriver)
}
