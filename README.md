# nlplayer-engine
A real-time audio playback engine for Android (an advanced fork of tinyalsa/tinyplay). Designed for audiophile-grade USB DAC output, it aggressively isolates the audio pipeline from Android OS jitter and bypasses standard alsa-lib blocking overhead to achieve a zero-syscall hot path.
