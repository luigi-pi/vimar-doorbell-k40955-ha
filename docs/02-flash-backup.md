# 2 — Back up the flash (do this first)

A full flash image is the only safe recovery if a write ever goes wrong. Take it **before** you change
anything. It is read‑only — nothing on the monitor is modified.

You need the root shell from [step 1](01-root-access.md). The dump is large; copy it off the monitor and
keep it somewhere safe. Never commit these files to git — they contain device‑unique keys and your
back‑label credentials.

## On the monitor

```sh
cat /proc/mtd
```

This lists the partitions. On this kit they are:

| Partition | Name | Size |
|---|---|---|
| mtd0 | uboot | |
| mtd1 | ubootenv | |
| mtd2 | kernel | |
| mtd3 | rootfs | 0x02800000 |
| mtd4 | data | 0x05500000 |
| mtd5 | all | 0x08000000 |

Dump the whole chip (and, for NAND, the spare/OOB area is the gold standard). Transfer each file to the
PC and record its `sha256sum`:

```sh
# on the monitor — adjust mtd numbers to YOUR /proc/mtd output
nanddump -f /tmp/nand_all.bin        /dev/mtd5     # whole chip
nanddump -of /tmp/oob_all.bin        /dev/mtd5     # with OOB (bad-block / ECC)
sha256sum /tmp/nand_all.bin /tmp/oob_all.bin
```

Copy them to the PC (for example with a quick HTTP server on the monitor and `wget` on the PC), then
**verify the checksums match** on both sides. Do not continue until you have verified copies on the PC.

> The one SSH‑enable edit from step 1 is recovered from the recovery boot without any reflash, so you will
> almost never need to write this image back. It is last‑resort insurance — keep it anyway.

## What is sensitive

The monitor's back label carries a **UID, default password, Wi‑Fi AP name and MAC**. The flash image and
the data partition contain these and the device's stream key. Keep all dumps off GitHub.
