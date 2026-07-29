# Flashing a repeater through a Raspberry Pi

Node is in another location, a Pi sits next to it on USB. Build on a workstation, flash from the Pi.

Notes from a Pi Zero 2 W with a XIAO ESP32S3.

## Don't build on the Pi

Zero 2 W has 416 MB RAM. A PlatformIO build of MeshCore there, alongside `hostapd`, `dnsmasq` and
Docker, reboots the board mid-compile.

## Steps

Build:

```bash
./build.sh --target xiao_s3_wio --build
```

Copy and check the image:

```bash
scp build/meshcore-upstream/.pio/build/Xiao_S3_WIO_repeater/firmware-merged.bin <pi>:/root/fw.bin
ssh <pi> 'md5sum /root/fw.bin'
md5 -q build/meshcore-upstream/.pio/build/Xiao_S3_WIO_repeater/firmware-merged.bin
```

Pick the right port. `ttyACM` numbering moves between boots, so match on USB serial:

```bash
ls -l /dev/serial/by-id/
udevadm info -q property -n /dev/ttyACM0 | grep -E 'ID_VENDOR|ID_MODEL|ID_SERIAL'
```

Flash:

```bash
systemctl stop docker meshtasticd ModemManager
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 115200 --no-stub write_flash 0x0 /root/fw.bin
systemctl start docker meshtasticd
```

Check:

```bash
curl -s http://<node-ip>/health
curl -s http://<node-ip>/stats
```

## Things that bite

**Host load breaks the write.** Everything on a Zero 2 W hangs off one `dwc_otg` controller. With
Docker and `meshtasticd` running, `esptool` dies with `The chip stopped responding` or
`Serial data stream stopped`, at a random offset. It reads like a flashing problem, so it pulls you
into lower baud rates, `--no-stub`, no compression, explicit `--flash_size`, esptool downgrades.
None of that helps. Stopping the services does: 1.4 MB in 41 s, hash verified.

If the Pi reboots during the write, same cause. `vcgencmd get_throttled` returning `0x0` rules out
undervoltage.

**ModemManager talks to serial ports.** It opens `ttyACM*` and pushes AT commands into them.
`systemctl stop` holds until the next reboot; mask it on a permanent flashing host.

**Detach the flash from SSH.** A dropped session kills the write and leaves the node half flashed:

```bash
setsid bash -c 'esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 115200 --no-stub \
  write_flash 0x0 /root/fw.bin > /root/flash.log 2>&1; echo EXIT=$? >> /root/flash.log; sync' \
  >/dev/null 2>&1 < /dev/null &
```

**A broken write is not a brick.** A partial image drops the ESP32-S3 into ROM download mode. The
node leaves the network, but `esptool` still connects and reads chip type and MAC. Just write again.

**Dump the config first.** Writing a merged image at `0x0` leaves the identity partition alone, and
here the key, name, radio and coordinates all survived. Partition layout can change upstream, so
save them anyway:

```
get prv.key
get public.key
get name
get radio
get tx
get lat
get lon
get repeat
```

Keep `prv.key` out of the repo. It is the node identity. The console port and HTTP panel have no
auth, so anyone on the same network can read it.

**WireGuard behind NAT needs `PersistentKeepalive = 25`.** Without it the NAT mapping expires while
the link is idle and the Pi is unreachable until it sends something. Losing the tunnel mid-write
leaves a half flashed node.

## Tested

2026-07-29, upstream MeshCore v1.16.0 built as `v1.16.0-extended`, XIAO ESP32S3 (`xiao_s3_wio`),
flashed from a Pi Zero 2 W with esptool 4.8.1 at 115200 and `--no-stub`. 1 491 968 bytes in 41.5 s,
hash verified.

Node came back with its public key, name, radio config and coordinates intact. WiFi, HTTP panel,
`/health`, `/stats` and MQTT publishing worked with no manual reconfiguration.
