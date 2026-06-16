# Makefile for ToHeart2 XRATED portable
#
# Usage:
#   make desktop          # configure and build the desktop executable
#   make android          # build the Android debug APK
#   make install-android  # install the debug APK on a connected device
#   make test             # run the desktop test suite
#   make clean            # remove all build artifacts

.PHONY: all desktop android install-android test clean clean-desktop clean-android logcat

# Default target builds both desktop and Android.
all: desktop android

# -----------------------------------------------------------------------------
# Desktop build
# -----------------------------------------------------------------------------

DESKTOP_BUILD_DIR := build
DESKTOP_TARGET := toheart2

# Release build with tests enabled.
desktop:
	cmake -S . -B $(DESKTOP_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	cmake --build $(DESKTOP_BUILD_DIR) --target $(DESKTOP_TARGET) -- -j$$(nproc)

# Run the desktop test suite.
test: desktop
	ctest --test-dir $(DESKTOP_BUILD_DIR) --output-on-failure

# Remove the desktop build tree.
clean-desktop:
	rm -rf $(DESKTOP_BUILD_DIR)

# -----------------------------------------------------------------------------
# Android build
# -----------------------------------------------------------------------------

ANDROID_BUILD_DIR := android
ANDROID_APK := $(ANDROID_BUILD_DIR)/app/build/outputs/apk/debug/app-debug.apk

# Build the Android debug APK using Gradle.
android:
	cd $(ANDROID_BUILD_DIR) && ./gradlew :app:assembleDebug

# Install the debug APK on the connected device.
install-android: android
	$(ANDROID_HOME)/platform-tools/adb install -r $(ANDROID_APK)

# Launch the app on the connected device and follow logcat.
logcat:
	$(ANDROID_HOME)/platform-tools/adb logcat -c
	cd $(ANDROID_BUILD_DIR) && ./gradlew :app:installDebug
	$(ANDROID_HOME)/platform-tools/adb shell am start -n io.github.ripdog.th2xr_portable/io.github.ripdog.th2xr_portable.MainActivity
	$(ANDROID_HOME)/platform-tools/adb logcat SDL/APP:D *:S

# Remove Android build artifacts.
clean-android:
	cd $(ANDROID_BUILD_DIR) && ./gradlew clean
	rm -rf $(ANDROID_BUILD_DIR)/app/build

# -----------------------------------------------------------------------------
# Global cleanup
# -----------------------------------------------------------------------------

clean: clean-desktop clean-android
