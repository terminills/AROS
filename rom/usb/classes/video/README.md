# UVC capture

The class binds UVC VideoControl functions to their associated VideoStreaming
interfaces. It enumerates YUY2, MJPEG, Y800/Y8 8-bit luminance and Y16 16-bit
little-endian luminance modes up to 3840x2160. Grayscale recognition requires
both the exact GUID and matching bits-per-pixel; unknown formats are rejected.
RGB and IR functions retain separate bindings even when product names match.
The mode picker selects a descriptor's exact format/frame pair, including
MJPEG and YUY2 modes with the same dimensions. There is no low-resolution
fallback after an explicit mode fails negotiation.

MJPEG uses the capturing task's `jfif.library` (version 52 or later), with
bounded input/output buffers, exact decoded-dimension checks and rejection of
truncated/corrupt frames. Grayscale frames are returned as RGB24 with equal
channels; Y16 display uses the high luminance byte. These implementation claims
do not establish physical IR-device compatibility without a matching device.

`uvc.library` exposes input 0 as an explicitly synthetic test pattern and inputs
1 onward as supported bound cameras. A selected input retains its binding ID;
an unplug cannot silently redirect it to another camera. Frames are RGB24.

The private COUNT/INFO/GRAB protocol uses the class's existing `usbDoMethodA`
vector. Existing VHI, USB-class and Poseidon vector positions are unchanged.
Descriptor ownership remains with Poseidon. Capture takes the device lock, with
no class lock held while waiting for it, and revalidates the binding afterward.

Capture queues 112 isochronous or two bulk requests, checks UVC FID/EOF/ERR headers,
and publishes only complete frames. Uncompressed frames require the expected
byte count; compressed frames require a nonempty bounded EOF frame and a
successful JPEG decode. Outstanding requests are
aborted and collected exactly once before buffers are freed. Capture has a
three-second deadline; alternate-setting cleanup gets one additional second.

## Viewer controls

VHITV captures in a separately loaded process so camera waits do not block
menus, resize, pause, or close. Closing drains the outstanding bounded request
before releasing shared data. Only one capture is in flight; frames made stale
by input, resolution, format, or interval changes are discarded.

- **Input**: select next/previous camera; information (`I`) shows VID/PID, USB
  speed, bulk/iso transport, VC/VS interfaces and the last streaming commit.
- **Camera modes**: exact advertised size and format.
- **Frame rate**: descriptor-advertised intervals for that mode. Continuous
  descriptors offer valid endpoints. The class rejects unadvertised intervals
  and a camera's refusal to negotiate the requested interval. Menu FPS is the
  stream interval, not a measured preview rate.
- **Device log** (`L`): separate live `log.resource` window, filtered by class
  and binding generation. It shows the latest twelve capture records.
- **Capture**: pause/resume. **Project** (`S`): full-frame PPM snapshot.

The metadata API is size-checked; old class/library pairs may lack these
extensions. Install the class, library and viewer as a matched set.

## Build and test

Initialize the upstream-pinned submodules before a fresh system build:

```
git submodule update --init --recursive
```

With an external GCC toolchain, build `linklibs-libatomic` into the private SDK
if it is absent. Run the normal `make` system target before `make bootiso`;
`bootiso` alone can package a kernel/test-only tree without a desktop.

Run the camera targets sequentially in a configured ADT build directory:

```
make -j8 kernel-usb-classes-uvc
make -j8 workbench-libs-vhi-uvc
make -j8 test-vhi
make -j8 workbench-tools-vhitv
```

The portable payload regression can be built with a host C compiler:

```
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
  test/payload.c -o payload-test
./payload-test
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
  test/formats.c -ljpeg -o formats-test
./formats-test
```

Install `uvc.class` in SYS:Classes/USB and `uvc.library` in LIBS:. Register the
class with AddUSBClasses (or Trident) and attach a supported camera. Run:

```
VHIProbe uvc.library RAM:synthetic.rgb 0
VHIProbe uvc.library RAM:camera.rgb 1
VHITV uvc.library 1
```

The first command is a synthetic ABI check, never camera evidence. The camera
probe must return a complete frame and the viewer must show scene changes to
establish real capture. Build or host regression success alone does not prove
USB transport, hot-unplug behavior, or a usable camera feed.

## Retained hardware validation

UGREEN Camera 2K (0c45:636f), USB high speed, YUY2 640x480 was exercised
through QEMU 10.2.1 xHCI passthrough on ADT x86-64 SMP. Two fresh VHIProbe
captures returned 921600-byte RGB24 images with different checksums, and
VHITV displayed the physical scene. Input zero's synthetic regression also
passed. This is camera-through-QEMU evidence, not bare-metal controller proof.

For this host, QEMU's default ISO buffering produced incomplete frames.
The successful usb-host settings were `guest-reset=false,isobufs=32,isobsize=128`.
Use only an explicitly selected camera and a currently verified host address.
The larger queue must be drained on shutdown; allow QEMU to exit before
starting another instance. The camera's Linux video/audio bindings recovered
between tests. No permanent permission rule is required.

The class captures one complete frame per call and stops the stream afterward.
The selected advertised camera rate is not the viewer's achieved frame rate;
persistent streaming and asynchronous viewer input handling remain future work.
Chromium camera enumeration and getUserMedia integration are not implemented
by this driver or viewer.
