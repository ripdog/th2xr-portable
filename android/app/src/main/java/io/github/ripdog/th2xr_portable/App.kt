package io.github.ripdog.th2xr_portable

import android.app.Application
import com.bugfender.sdk.Bugfender

class App : Application() {

    override fun onCreate() {
        super.onCreate()

        Bugfender.init(this, "rz9SGOIC1tXgC1xrKoQMVuc9LCQIeMOS", BuildConfig.DEBUG)
        Bugfender.enableCrashReporting()
        Bugfender.enableLogcatLogging()
        Bugfender.d("Bugfender", "SDK initialized")
    }
}
