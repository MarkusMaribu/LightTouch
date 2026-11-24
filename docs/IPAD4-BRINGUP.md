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

Set `QEMU_SRC` to the patched engine source checkout. On Windows, run the build in MSYS2 UCRT64 and use the `qemu-system-arm.exe` target. Add `--enable-sdl` and SDL2 to the build dependencies for a desktop display window.

```sh
"$QEMU" -L "$QEMU_BIOS" -M "iPad-4-wifi,kboot=$OUT/restore.kboot" -m 1G \
  -display none -monitor none -audio driver=none -no-reboot \
  -serial "file:$OUT/serial.log" -d guest_errors,unimp -D "$OUT/mmio.log"
```

The A6X device tree shares the A6 interrupt controller, Swift CPU architecture, and UART6 address. The port uses the D2018's I²C address and interrupt from the firmware. Its experimental eDP controller uses the shared QEMU DisplayPort sink and a 2048 × 1536 virtual panel. [QEMU-iOS A6 board notes](https://github.com/samhenrigold/qemu-ios/blob/main/docs/n42/README.md)

In a Windows headless test, the stock kernel mounted `md0` and started `launchd` and `restored_external`. With disposable PPN media, the stock flash driver formatted the NAND, reported `VFL_Open [OK]` and `FTL_Open [OK]`, and published a 16 GB block device.

## Install the System volume

Build the source-only `it_initialize` helper from the pinned engine checkout.
With the engine's 32-bit iOS build toolchain configured, set `RESTORE_STOCK_DATA_ONLY=1` when running
`contrib/it-restore/build.sh` to build a helper that copies the stock Data seed directly from System.
Set `INITIALIZER` to the path of that built binary.

Python 3.11 or later can run the full restore and launch sequence. Use a new directory for `DEVICE`.

```sh
python3 scripts/ipad4.py prepare --ipsw "$IPSW" --device "$DEVICE" \
  --initializer "$INITIALIZER" --qemu "$QEMU" --bios "$QEMU_BIOS"
python3 scripts/ipad4.py run --device "$DEVICE" --qemu "$QEMU" --bios "$QEMU_BIOS"
```

The launcher defaults to SDL. Choose `--display cocoa` if the macOS engine was built with Cocoa. The `Home` key maps to the iPad's Home button.
Pass `--offline-setup` to `prepare` to seed iOS 6's Setup Assistant and factory activation preferences for the virtual device.
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

The installed System starts stock `launchd`, and both volumes mount. A cold boot loads the Data keybag. Windows tests captured the iOS first-boot progress screen and the 2048 × 1536 lock screen. With `--offline-setup`, the lock screen displays “slide to unlock,” and the native touch driver receives drag input. A timeout in the media service can restart SpringBoard. Reaching the Home screen and launching apps are still unverified.
