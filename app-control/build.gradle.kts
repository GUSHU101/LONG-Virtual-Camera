plugins {
    id("com.android.application")
}

val ciKeystorePath = System.getenv("LONG_KEYSTORE_PATH")

android {
    namespace = "io.github.longcontroller"
    compileSdk = 35
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "io.github.longcontroller"
        minSdk = 33
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-alpha01"

        testInstrumentationRunner = "android.test.InstrumentationTestRunner"
    }

    signingConfigs {
        create("ciRelease") {
            if (!ciKeystorePath.isNullOrBlank()) {
                storeFile = file(ciKeystorePath)
                storePassword = System.getenv("LONG_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("LONG_KEY_ALIAS")
                keyPassword = System.getenv("LONG_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            buildConfigField("boolean", "EXPERIMENTAL_BUILD", "true")
        }
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            buildConfigField("boolean", "EXPERIMENTAL_BUILD", "true")
            if (!ciKeystorePath.isNullOrBlank()) {
                signingConfig = signingConfigs.getByName("ciRelease")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = true
    }

    testOptions {
        unitTests.isIncludeAndroidResources = false
    }

    lint {
        abortOnError = true
        checkReleaseBuilds = true
        warningsAsErrors = false
        disable += setOf("OldTargetApi")
    }

}

tasks.withType<JavaCompile>().configureEach { options.encoding = "UTF-8" }

dependencies {
    compileOnly("de.robv.android.xposed:api:82")
    testImplementation("junit:junit:4.13.2")
}
