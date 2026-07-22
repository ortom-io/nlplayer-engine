# nlplayer-engine

A highly optimized, real-time audio playback engine for Android, built specifically for bit-perfect output via USB DACs. 

This project provides a custom audio player (`tinyplay`) and a complete cross-compilation environment. It compiles fully static binaries (TinyALSA + FFmpeg) heavily optimized for ARM64 (Cortex-A78) using advanced instruction sets, thread isolation, and kernel-bypass techniques to achieve zero-latency playback.

## 🚀 Technical Highlights

* **Direct Audio Output:** Bypasses the Android system audio mixer, communicating directly with USB DACs via ALSA.
* **CPU Core Isolation:** Utilizes `cgroups` to isolate dedicated CPU cores exclusively for real-time audio processing.
* **Lock-Free Architecture:** Implements Shared Memory (SHM) and `eventfd` for zero-syscall, non-blocking playback control.
* **Fully Static Binaries:** ALSA-lib, FFmpeg, and TinyALSA are linked statically, ensuring zero dependencies on the Android host OS libraries.

---

## 🛠 Prerequisites

### 1. Android NDK
The **Android NDK** is required for cross-compilation. 
* If you have **Android Studio** installed, the NDK is usually installed automatically. 
* Alternatively, you can install it manually (e.g., via your package manager or by downloading it from Google). 
* **Note:** You do not need to configure any custom paths or environment variables. The build script features a heuristic discovery engine that will automatically locate the NDK on your system.

### 2. System Dependencies
You will need standard development tools, autotools, and 7-Zip.

**For Arch Linux:**
```bash
sudo pacman -S base-devel git autoconf automake libtool p7zip curl wget
```
*(For Debian/Ubuntu, use `apt install build-essential git autoconf automake libtool p7zip-full curl wget`)*

---

## 🏗 Build Instructions

Compiling the project is entirely automated. Just clone the repository, make the build script executable, and run it:

```bash
git clone https://github.com/ortom-io/nlplayer-engine.git
cd nlplayer-engine
chmod +x build.sh
./build.sh
```

The script will automatically fetch the required source trees (ALSA-lib, TinyALSA, FFmpeg), inject the custom `tinyplay.c` engine, and perform a highly optimized static build.

### 📦 Build Artifacts

Upon successful compilation, the script will generate a compressed archive containing the stripped static binaries (`tinyplay`, `ffmpeg`) and necessary ALSA configuration files (`share`). 

You can find the final deployment payload here:
```text
android_build/android_static_binaries/bin/tools.7z
```

---

## ⚖️ License & Commercial Licensing

This project is **dual-licensed**:

1. **Open Source Use (GPLv3):** `nlplayer-engine` is available under the terms of the [GNU General Public License v3.0](LICENSE) for open-source software, personal use, and non-commercial research.
2. **Commercial Use:** If you wish to integrate `nlplayer-engine` into a proprietary, closed-source commercial application, custom Android ROM, or hardware device without releasing your source code under GPLv3, a commercial license must be purchased.

For commercial licensing inquiries:
- **Email:** `ortom.dev@proton.me`
- **Telegram:** `@ortom_io`
