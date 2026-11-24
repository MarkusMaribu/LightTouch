# iPad 4 Wi-Fi boot bring-up

The target is the fourth-generation Wi-Fi iPad (`iPad3,4`, `p101ap`, A6X) on iOS 6.1.3 (`10B329`).
FirmwareKit can extract the matching kernel and restore ramdisk and build an A6X boot bundle from the stock IPSW.
The catalog keeps this build out of the desktop and Android setup screens while the emulator and preparation recipe are incomplete.

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
"$QEMU" -L "$QEMU_BIOS" -M "iPad-4-wifi,kboot=$OUT/restore.kboot" -m 1G \
  -display none -monitor none -audio driver=none -no-reboot \
  -serial "file:$OUT/serial.log" -d guest_errors,unimp -D "$OUT/mmio.log"
```

The A6X device tree shares the A6 interrupt controller, Swift CPU architecture, and UART6 address. The port uses the D2018's I²C address and interrupt from the firmware. The native eDP display and normal-boot storage provisioning need further work. [QEMU-iOS A6 board notes](https://github.com/samhenrigold/qemu-ios/blob/main/docs/n42/README.md)

In a Windows headless test, the stock kernel mounted `md0` and started `launchd` and `restored_external`. With disposable PPN media, the stock flash driver formatted the NAND, reported `VFL_Open [OK]` and `FTL_Open [OK]`, and published a 16 GB block device. System and Data provisioning and a normal Home-screen boot remain unverified.
