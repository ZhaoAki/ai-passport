<p align="right"><a href="cyber-lanlan-character.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Lanlan character update

This v0.2 visual update replaces the geometric placeholder with the previously
established Lanlan identity: cream drop ears, a white muzzle and chest, round dark
eyes and a fluffy crown. The owner's 12 GIFs inform expressions and gestures.
Built-in imagegen produced six frames, retained with the exact prompt under
`assets/images/lanlan-v2/`. Visual acceptance by the owner is pending.

The companion now has two idle frames, closed-eye blink, a brief spontaneous head
tilt, happy raised paws and an open-mouth bark expression. Automatic idle reactions
alternate with blinking; existing OK interaction and record-independent behavior
remain. The phone overview and profile use the same generated character.

The device still uses a single 96 × 96 RGB565 canvas, 18,432 bytes of RAM. Six
constant frames occupy 110,592 bytes in Flash, 18,432 more than v0.1. Runtime GIF
decoding is unnecessary. The captions in the reference stickers are not embedded.
No food, drink or care record is created by an animation.

Asset checks compare source/converted hashes, compiled frame bytes, six distinct
poses and fixed memory dimensions. The host UI stress run passes 1,103 page
switches, 2,160 key events and 2,880 renders; LVGL free memory returns to 13,432
bytes with a 10,328-byte minimum. The complete repository gate was attempted;
HTTP tests remain blocked by the environment's local socket permission. Firmware
build and layout verification are reported in the release manifest. On-device
appearance and timing still require an authorized update and owner observation.

Use the new source's `tools/start-lanlan-local.command` for the phone portrait;
it keeps the same local database path as v0.1. The v0.1 release remains available.
The new merged firmware may reset device settings and cache; flashing is a separate
authorized action. No Git push, cloud deployment or device write is part of this
visual update.
