# nlplayer-engine

A highly optimized audio playback engine for Android, built specifically to achieve the **highest possible audio output quality** and bit-perfect fidelity via USB DACs.

This project provides a custom audio player (`tinyplay`) and an automated cross-compilation environment. It compiles fully static binaries (ALSA-lib + FFmpeg) heavily optimized for ARM64 (Cortex-A78) using advanced instruction sets, thread isolation, and kernel-level optimizations to eliminate system jitter and deliver uncompromised audio performance on Android smartphones.

> ⚠️ **Requirement on Target Device:** Running `nlplayer-engine` on an Android device **requires Root access**. The engine directly manipulates kernel parameters (`/sys`, `/proc`), CPU `cgroups`, memory locking (`mlockall`), and hardware interfaces to guarantee maximum fidelity.

---

## 🚀 Technical Highlights

* **Maximum Audio Fidelity:** Bypasses Android's `AudioFlinger` and software resamplers completely, delivering pure, bit-perfect digital audio directly to external USB DACs.
* **Full ALSA-lib Backend:** Integrates full `alsa-lib` (`libasound`), allowing native control over hardware sample rates, bit depths, and buffer alignment.
* **System Jitter & Stutter Elimination:** Uses Linux `cgroups` for strict CPU core isolation and real-time process scheduling (`SCHED_FIFO`) to prevent background Android processes from interfering with audio streaming.
* **Lock-Free Architecture:** Implements Shared Memory (SHM) and `eventfd` IPC for zero-syscall, non-blocking playback control.
* **Fully Static Binaries:** Built as standalone static binaries with no external dependencies on Android host OS libraries.

---

## 🛠 Prerequisites for Building

### 1. Android NDK
The **Android NDK** is required for cross-compilation. 
* If you have **Android Studio** installed, the NDK is usually installed automatically. 
* Alternatively, you can install it manually (e.g., via your package manager or by downloading it from Google). 
* **Note:** You do not need to configure any custom paths or environment variables. The build script features a heuristic discovery engine that will automatically locate the NDK on your system.

### 2. System Dependencies
You will need standard C/C++ build tools, GNU autotools, and 7-Zip.

**Arch Linux:**
```bash
sudo pacman -S --needed base-devel git autoconf automake libtool p7zip curl wget
```

**Ubuntu & Debian:**
```bash
sudo apt update && sudo apt install -y build-essential git autoconf automake libtool p7zip-full curl wget
```

---

## 🏗 Build Instructions

Compiling the project is entirely automated. Just clone the repository, make the build script executable, and run it:

```bash
git clone https://github.com/ortom-io/nlplayer-engine.git
cd nlplayer-engine
chmod +x build.sh
./build.sh
```

The script will automatically fetch the required source trees (ALSA-lib, FFmpeg, and the base player sources), inject the custom playback engine, and perform a highly optimized static build.

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