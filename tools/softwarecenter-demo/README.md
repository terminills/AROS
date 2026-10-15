# Software Center live demo

Run `prepare.py --publisher /path/to/arospkg-server --output NEW_DIRECTORY`.
The default guest URL is `http://10.0.2.2:18096/`, the QEMU user-network host.
Use `--url` with the demo server's LAN address for a hardware guest.

Serve the generated `repo` directory with arospkgd on port 18096. For QEMU,
bind only to 127.0.0.1; for hardware, use the intended LAN interface.
The key, signed repositories, archives and build log stay together in the
output directory. Only `payload/SoftwareCenterDemo` goes on the image, at
`SYS:SoftwareCenterDemo`; it contains the public key and offline baseline,
never the private signing key.

Add `Execute SYS:SoftwareCenterDemo/Startup` to S:User-Startup before LoadWB.
That installs AmiWestWelcome 1.0.0 into RAM:, configures the remote source and
sets the notifier's context before WBStartup creates it. Do not asynchronously
change that context after the notifier has started. Do not start a second
notifier: the image's normal WBStartup instance performs its first check after
its configured delay (120 seconds by default).

Demonstration: fresh boot, notification, Open Software Center, update
AmiWestWelcome to 1.0.1, install AmiWestTools, then remove it. All package state
and installed files are disposable; reboot restores the baseline. The startup
script preserves a running demo when executed again.

Validation requires screenshots of the GUI sequence and the server/client
logs. Generating and signing this fixture alone does not establish a GUI pass.
