# Makefile for ToHeart2 XRATED portable
#
# Usage:
#   make desktop          # configure and build via CMake preset (does not test)
#   make run              # build and run the desktop executable
#   make android          # build the Android debug APK
#   make install-android  # install the debug APK on a connected device
#   make test             # build and run the desktop test suite
#   make clean            # remove all build artifacts

.PHONY: all desktop run android install-android test clean clean-desktop clean-android logcat

# Default target builds both desktop and Android.
all: desktop android

# -----------------------------------------------------------------------------
# Desktop build
# -----------------------------------------------------------------------------

DESKTOP_BUILD_DIR := build/release
DESKTOP_TARGET := toheart2
GAME_DATA_DIR ?= game-data

# Configure and build (no tests).
desktop:
	cmake --preset desktop-release
	cmake --build --preset desktop-release

# Build and run the desktop executable.
run: desktop
	./$(DESKTOP_BUILD_DIR)/$(DESKTOP_TARGET) $(GAME_DATA_DIR)

# Build and test.
test: desktop
	ctest --preset desktop-release

# Remove the preset and manual build trees.
clean-desktop:
	rm -rf build/

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
