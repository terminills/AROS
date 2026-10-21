# USB microphone capture validation

`AudioCaptureProbe.c` is a bounded native AHI integration test. Build with the
AROS cross compiler and the matching SDK, for example:

```
x86_64-aros-gcc --sysroot="$AROS_SDK" -o AudioCaptureProbe AudioCaptureProbe.c
```

Run `AudioCaptureProbe` after Poseidon has bound a USB audio device. It selects
an input-capable mode in the USB audio ID family, requests 48 kHz stereo 16-bit
capture, records for five seconds, stops, verifies that callbacks cease, frees
the allocation, and repeats. Missing modes, allocation/start failures, empty
captures, or callbacks after stop fail the test. Nonzero sample counts and peak
levels are reported separately; silence alone is not a transport failure.

With `HEX`, the test exports the first second of its second capture as
little-endian stereo PCM between `PCM_BEGIN` and `PCM_END`. Redirect stdout to
`SER:` to retrieve the payload. Export happens after recording has stopped;
no logging or file I/O runs inside the recording hook.

## Hardware validation

The UGREEN Camera 2K (0c45:636f), passed through to the private native x86_64 SMP
QEMU guest, exposes a microphone-only UAC1 interface: two channels, 16 bits,
48 kHz, endpoint 0x84, 192 bytes per 1 ms service interval. Before the fix it
was omitted from AHI because no matching playback interface existed.

The fixed driver registers standalone capture modes, selects recording
converters, addresses input endpoint controls correctly, and balances stream
starts and stops. xHCI realtime scheduling consumes Poseidon's normalized
interval and distinguishes automatic scheduling from client-selected frames.

Two clean five-second captures and an allocation reopen passed with nonzero
PCM, and a one-second WAV was recovered through the serial export. Camera
video was restored afterwards. This validates microphone capture through
AHI in the passthrough guest, not bare-metal USB, synchronized simultaneous
video/audio, Chromium/WebRTC integration, or every supported PCM format.
