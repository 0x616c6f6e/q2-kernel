# Audio playback

Kernel provides an optional `audio.playback` component through `q2_audio_backend_t` (ABI 1).
The platform owns networking, decoding, the output device and worker lifetime. Kernel owns permission checks,
URL staging and exclusive APP-instance ownership. All owner callbacks are bounded; workers never call Lua/Kernel.

## Lua API 2

All methods require `audio.playback`. URL playback additionally requires the caller's `network.http` grant.
Methods without a result return `true`; failures return `nil, {code, number}`.

| Method | Meaning |
| --- | --- |
| `q.audio.open()` | Claim the player and start a new URL draft; cancels this owner's previous track. Another owner returns busy. |
| `q.audio.write(text)` | Append up to 255 bytes to the draft, at most 2048 bytes total. |
| `q.audio.play()` | Start the completed HTTP/HTTPS URL asynchronously. Backend busy preserves the draft for a later retry. |
| `q.audio.pause(true/false)` | Request pause/resume; poll state for acknowledgement. |
| `q.audio.state()` | idle, buffering, playing, paused or ended; decoder/transport errors return the error result. |
| `q.audio.position()` | Real decoder position in milliseconds. |
| `q.audio.duration()` | Actual media duration in milliseconds, initially zero. |
| `q.audio.stop()` | Cancel and release ownership; idempotent if unowned. |

No arbitrary local paths or URL user-info are accepted. A different APP cannot inspect, stop or append to the
owner's playback. APP exit/release automatically cancels audio. Explicit Home navigation without exiting the VM
retains ownership, enabling future background players. Physical input policy remains in Lua APPs.

## Windows provider

`platform/x86/audio_win32.c` runs synchronous WinHTTP and Windows MCI on a dedicated STA worker. The UI thread
only copies commands/snapshots. It downloads a bounded MP3/WAV to a temporary file, then decodes and plays through
the default Windows audio device. This version buffers the whole file before playback; it is not incremental
streaming. Maximum file size is 32 MiB, download deadline 30 seconds, no browser cookies/implicit authentication,
no redirect following, and normal HTTPS certificate verification. Signed URLs are never logged. Temporary files
are deleted on stop/completion/failure. Abrupt OS/process termination can leave a temporary file.

The component switch is `components.audio` in root q2.json. Music also needs network and storage for its normal
workflow. There is no ESP32/I2S/CS43131 implementation yet; this provider validates PC playback, not hardware.

## Verification

`Q2_BUILD_AUDIO_TESTS=ON` enables `q2_audio_contracts`: ownership, permissions, URL limits, stop/release and state.
`app/scripts/test_ncm_ui.py` serves locally generated silent MPEG-1 Layer III frames and exercises real Windows
MP3 decoding/output, pause/resume, end-of-track, cancellation and restart with temporary login storage.

Native API reference: [MCI_OPEN](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-open).
