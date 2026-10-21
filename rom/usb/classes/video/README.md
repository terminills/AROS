# UVC capture

The class binds UVC VideoControl interfaces with a YUY2 streaming mode no larger
than 640x480. It selects the smallest such mode, retains that frame's interval,
and negotiates the format before capture. MJPEG and other uncompressed GUIDs
are deliberately unsupported. Isochronous endpoint capacity includes the
high-speed transaction multiplier exposed separately by Poseidon.

`uvc.library` exposes input 0 as an explicitly synthetic test pattern and inputs
1 onward as supported bound cameras. A selected input retains its binding ID;
an unplug cannot silently redirect it to another camera. Frames are RGB24.

The private COUNT/INFO/GRAB protocol uses the class's existing `usbDoMethodA`
vector. Existing VHI, USB-class and Poseidon vector positions are unchanged.
Descriptor ownership remains with Poseidon. Capture takes the device lock, with
no class lock held while waiting for it, and revalidates the binding afterward.

Capture queues 16 isochronous or two bulk requests, checks UVC FID/EOF/ERR headers,
and publishes only an exact-size complete frame. Outstanding requests are
aborted and collected exactly once before buffers are freed. Capture has a
three-second deadline; alternate-setting cleanup gets one additional second.

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
