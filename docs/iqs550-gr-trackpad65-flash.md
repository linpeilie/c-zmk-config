# GR-Trackpad65 IQS550 flashing

This repository includes a one-shot ZMK build that programs the GR-Trackpad65
IQS550 image without a CT210A.

## Build artifact

GitHub Actions builds this target:

```yaml
board: nice_nano_v2
shield: futaba futaba_iqs550_flash
artifact-name: futaba_iqs550_flash
```

The target reuses the normal `futaba` keyboard shield and overlays
`program-firmware` on the `iqs550` node. It also enables USB logging.

## Flash procedure

1. Flash the `futaba_iqs550_flash` UF2 to the nRF52840.
2. Open the USB serial log and wait for `GR-Trackpad65 IQS550 firmware programmed successfully`.
3. Flash the normal `futaba` UF2 back to the nRF52840.

The writer first enters the IQS5xx I2C bootloader at `0x34` (`0x74 ^ 0x40`).
If the device is already running an application and no `reset-gpios` is routed,
the driver requests an IQS5xx software reset over I2C and then polls the
bootloader again.

By default the writer reads back the `0xBE00-0xBFFF` non-volatile configuration
area and skips reprogramming when it already matches the bundled image.

## Failure hints

- If the log shows bootloader entry failure, power-cycle the keyboard while the
  writer firmware is already running.
- If the chip still cannot enter bootloader, temporarily expose IQS550 `NRST`
  to the nRF52840 and add a `reset-gpios` property to the `iqs550` node.
- If CRC fails, check I2C wiring, pull-ups, power, and whether the IQS550 is a
  bootloader-capable `IQS550BLQNR`.
