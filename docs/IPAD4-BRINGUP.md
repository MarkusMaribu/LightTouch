# iPad 4 Wi-Fi boot bring-up

The target is the fourth-generation Wi-Fi iPad (`iPad3,4`, `p101ap`, A6X) on iOS 6.1.3 (`10B329`).
FirmwareKit can extract the matching kernel and restore ramdisk and build an A6X boot bundle from the stock IPSW.
The catalog keeps this build out of the desktop and Android setup screens while the engine and preparation recipe are in development.

## Prepare a restore boot bundle

Run these commands from the LightTouch checkout with `IPSW` set to the path of `iPad3,4_6.1.3_10B329_Restore.ipsw`.
The expected SHA-256 is `fed2ecb8f0f530437247f755a5f1533a21517992a6a212af96d6a2adee4f0e4a`.
Choose an `OUT` directory outside both source checkouts for the extracted firmware.

```sh
cargo build --locked -p lighttouch-firmware --bin firmwarekit
FWKIT=target/debug/firmwarekit

"$FWKIT" decrypt --catalog Portable/firmware/firmware-catalog.json --id p101ap-10B329 \
  --ipsw "$IPSW" --out "$OUT/decrypted" --no-rootfs
"$FWKIT" extract-rootfs "$OUT/decrypted/048-2666-005-ramdisk.dmg" "$OUT/restore.hfs"
"$FWKIT" kboot --decrypted "$OUT/decrypted" --seed p101ap-10B329 \
  --ramdisk "$OUT/restore.hfs" --boot-args '-v serial=3 debug=0x8' --out "$OUT/restore.kboot"
```

## Run a serial boot probe

Apply the [engine patch](../build-support/patches/qemu-ios-p101.patch) to QEMU-iOS at
`db230fe8e8e4d297724b2eccdc38bdc21876696a` from [samhenrigold/qemu-ios](https://github.com/samhenrigold/qemu-ios).
The patch adds the experimental `iPad-4-wifi` machine and the host changes used for the Windows build.
Set `QEMU` to the resulting `qemu-system-arm` executable and `QEMU_BIOS` to the engine checkout's `pc-bios` directory.

```sh
mkdir qemu-build
(
  cd qemu-build
  "$QEMU_SRC/configure" --target-list=arm-softmmu --without-default-features \
    --without-default-devices --with-devices-arm=lighttouch-a6 --enable-tcg \
    --enable-pixman --enable-gcrypt --disable-werror --disable-docs --disable-tools \
    --disable-guest-agent --disable-plugins --audio-drv-list= --extra-ldflags=-lcrypto
  ninja qemu-system-arm
)
```

Set `QEMU_SRC` to the patched engine source checkout. On Windows, run the build in MSYS2 UCRT64 and use the `qemu-system-arm.exe` target. For an interactive Windows build, install SDL2 and libslirp, add `--enable-sdl --enable-slirp` and set `--audio-drv-list=sdl`. The engine builds its pinned libslirp subproject when the system library lacks its network controls. A Cocoa build uses the Core Audio driver.

```sh
"$QEMU" -L "$QEMU_BIOS" -M "iPad-4-wifi,kboot=$OUT/restore.kboot" -m 1G \
  -display none -monitor none -audio driver=none -no-reboot \
  -serial "file:$OUT/serial.log" -d guest_errors,unimp -D "$OUT/mmio.log"
```

The A6X device tree shares the A6 interrupt controller, Swift CPU architecture, and UART6 address. The port uses the D2018's I²C address and interrupt from the firmware. Its experimental eDP controller uses the shared QEMU DisplayPort sink and a 1536 × 2048 portrait panel. [QEMU-iOS A6 board notes](https://github.com/samhenrigold/qemu-ios/blob/main/docs/n42/README.md)

In a Windows headless test, the stock kernel mounted `md0` and started `launchd` and `restored_external`. With disposable PPN media, the stock flash driver formatted the NAND, reported `VFL_Open [OK]` and `FTL_Open [OK]`, and published a 16 GB block device.

## Install the System volume

Build the source-only `it_initialize` helper from the pinned engine checkout.
With the engine's 32-bit iOS build toolchain configured, set `RESTORE_STOCK_DATA_ONLY=1` when running
`contrib/it-restore/build.sh` to build a helper that copies the stock Data seed directly from System.
Set `INITIALIZER` to the path of that built binary.

Python 3.11 or later can run the full restore and launch sequence. Use a new directory for `DEVICE`.

```sh
python3 scripts/ipad4.py prepare --ipsw "$IPSW" --device "$DEVICE" \
  --initializer "$INITIALIZER" --qemu "$QEMU" --bios "$QEMU_BIOS" \
  --offline-setup
python3 scripts/ipad4.py run --device "$DEVICE" --qemu "$QEMU" --bios "$QEMU_BIOS"
```

The launcher defaults to SDL. The SDL window fits the monitor at startup, and dragging an edge or corner scales the iPad display while keeping its aspect ratio. `Ctrl+Alt+F` toggles full screen. Choose `--display cocoa` if the macOS engine was built with Cocoa. The iPad wakes once if its display sleeps during startup. Click the sleep screen to wake it again. The `Home` key maps to the iPad's Home button.

The iPad starts in portrait at 1536 × 2048 with a 768 × 1024 point layout. Resize the window for a smaller picture. iOS still uses its Retina artwork. The experimental `--low-resolution` option changes the guest panel to 768 × 1024. The iPad 4 firmware omits 1x versions of some icons and controls, which appear blank in that mode. Photo Booth opens and draws its effect picker. Its live previews are currently unavailable on Windows.

`--offline-setup` seeds iOS 6's Setup Assistant and factory activation preferences for the virtual device.
Interactive launches enable the emulated codec and use the host’s default audio devices. The iPad opens the microphone when it starts capture and releases it when capture stops. Headless runs use the silent backend. The optional `--silent-audio` preparation switch moves the stock `VirtualAudio` plug-in out of its discovery directory and disables guest microphone support.
The restore command checks the IPSW hash and every System record's checksum, reads back the final record and compares the Data keybag before and after a remount.
The device directory holds the writable virtual NAND, boot bundle and serial logs.

For individual preparation steps, use FirmwareKit directly.

```sh
"$FWKIT" decrypt --catalog Portable/firmware/firmware-catalog.json --id p101ap-10B329 \
  --ipsw "$IPSW" --out "$OUT/decrypted"
"$FWKIT" extract-rootfs "$OUT/decrypted/rootfs.dmg" "$OUT/system.hfs"
"$FWKIT" a6-nand "$OUT/nand"
"$FWKIT" a6-stream "$OUT/system.hfs" "$OUT/system.a6di"
cp "$OUT/restore.hfs" "$OUT/initialize.hfs"
"$FWKIT" hfs-grow "$OUT/initialize.hfs" 67108864
"$FWKIT" hfs-edit "$OUT/initialize.hfs" 'rm:/usr/local/bin/restored_external' \
  "write:/usr/local/bin/restored_external=$INITIALIZER" 'chmod:/usr/local/bin/restored_external=0755'
"$FWKIT" kboot --decrypted "$OUT/decrypted" --seed p101ap-10B329 \
  --ramdisk "$OUT/initialize.hfs" \
  --boot-args '-v serial=3 debug=0x8 nand-enable-reformat=1 amfi_allow_any_signature=1 cs_enforcement_disable=1' \
  --out "$OUT/initialize.kboot"
"$QEMU" -L "$QEMU_BIOS" \
  -M "iPad-4-wifi,kboot=$OUT/initialize.kboot,ppn=on,nand=$OUT/nand,restore-image=$OUT/system.a6di" \
  -m 1G -display none -monitor none -audio driver=none -no-reboot -serial "file:$OUT/initialize.log"
```

The helper writes through the native flash driver and provisions Data.
The tested restore logged `A6RESTORE DONE records=23284 bytes=1525833728` and a clean unmount of both volumes.
A separate read-only pass through the same flash driver verified all 23,284 records against the stock image.
The temporary ramdisk uses signing options for the locally built restore helper. A normal boot uses the original System image and omits those options.

```sh
"$FWKIT" kboot --decrypted "$OUT/decrypted" --seed p101ap-10B329 \
  --boot-args '-v serial=3 debug=0x8 rd=disk0s1' --out "$OUT/normal.kboot"
"$QEMU" -L "$QEMU_BIOS" \
  -M "iPad-4-wifi,kboot=$OUT/normal.kboot,ppn=on,nand=$OUT/nand,display-probe=on" \
  -m 1G -display none -monitor none -audio driver=none -no-reboot -serial "file:$OUT/normal.log"
```

The installed System starts stock `launchd`, and both volumes mount. A cold boot loads the Data keybag. Windows tests verified the 1536 × 2048 lock screen, a drag to unlock, and a tap on the Home screen that opened Photo Booth. The Windows package leaves camera emulation off, so Photo Booth's previews are blank.

On a separate test device with the stock VirtualAudio plug-in, enabling `audio-ports-probe=on,tristar-probe=on,ae2-probe=on,camera=on` lets the iOS media service start. The virtual front camera delivers 1280 × 960 frames. Photo Booth requests an OpenGL ES 2 context to draw its effects. Context creation returns nil on the Windows engine. Core Video then rejects texture-cache creation with error −6661 and Photo Booth exits. Keep camera emulation off for this build.

### Windows launcher

From the repository root, build the launcher with a 64-bit MinGW-w64 toolchain.

```sh
windres Windows/ipad4/launcher.rc -o launcher.res.o
gcc -O2 -municode -mwindows Windows/ipad4/launcher.c launcher.res.o -o LightTouch-iPad4.exe
```

Place the EXE beside the prepared `device` folder and a `runtime` folder containing the engine as `qemu-system-arm-display.exe`, its DLLs, and `pc-bios`. Double-click the EXE for a resizable Retina window.

Press **F6** in the SDL window to alternate between portrait and landscape. This turns the virtual motion sensors and the display together. Clicks and drags follow the displayed controls. The window swaps its width and height and fits within the current monitor. Maximized and full-screen windows keep their bounds.

Press **F7** in the emulator window to put the iPad to sleep. Press **F7** again to wake it at the iOS lock screen. Holding the key triggers the shortcut once.

The iPad’s Wi-Fi network is named **qemu-ios**. Open **Settings → Wi-Fi** and select it if the iPad has not joined automatically. The blue arrow beside the selected network opens its DHCP details. The virtual Broadcom BCM4334 connects to the host’s internet connection through QEMU user-mode networking. The launcher uses `lan=off` to keep the host’s local network private.

To turn on Siri, open **Settings → General → Siri**, turn the switch on, then tap **Enable**. The test used **English (United States)**. Press **F8** in the SDL window or hold **Home** for about two seconds. Both gestures invoke the iPad’s Home-button hold. On an offline iPad, Siri shows “Connect to the Internet.” iOS 6 sends speech to a server for recognition. [Apple’s iOS 6.1 iPad guide, page 40](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1659_ipad_ios6_user_guide.pdf#page=40)

On a connected test iPad, Siri displayed “What can I help you with?” and the guest audio driver received a synthesized “Tell me a joke” through the microphone. With the factory trust store, the request stopped during TLS negotiation. Safari on the same iPad reported that it could not verify the certificate for `guzzoni.apple.com`. The certificate's issuer was Apple Public EV Server RSA CA 1 - G1, which chains to DigiCert Global Root G2. [Apple’s certificate validation page](https://valid-gr2-rsa.apple.com/)

On a disposable iPad, installing DigiCert Global Root G2 through Settings let Safari establish a verified HTTPS connection to that hostname. Siri still displayed “I can’t take any requests right now.” A cold boot and a separate synthesized “Hello” produced the same result. Both test recordings reached the guest’s audio driver. Spoken replies remain unverified, and the Windows package uses the iPad’s existing trust settings. [DigiCert’s published root fingerprints](https://knowledge.digicert.com/general-information/digicert-trusted-root-authority-certificates)

The Windows upgrade includes a locally built `runtime/repair-audio.kboot` for iPads prepared with `--silent-audio`. On first launch it checks the stock 10B329 plug-in, moves it back into the HAL directory if needed, and verifies it after a read-only remount. It mounts only the System volume. The launcher writes `device/nand/audio-hal-v1.ok` after verification and clean guest shutdown. See `device/repair-audio.log` for the result.

For a source build, compile `scripts/ipad4/repair_audio.c` with `scripts/ipad4/build-repair-helper.ps1` and the same ARMv7 toolchain used for the restore helper. With the helper in `AUDIO_HELPER`, use the following commands on a stopped device.

```sh
python3 scripts/ipad4.py build-audio-repair --decrypted "$OUT/decrypted" \
  --helper "$AUDIO_HELPER" --out "$OUT/repair-audio.kboot" --firmwarekit "$FWKIT"
python3 scripts/ipad4.py repair-audio --repair-boot "$OUT/repair-audio.kboot" \
  --device "$DEVICE" --qemu "$QEMU" --bios "$QEMU_BIOS"
```
