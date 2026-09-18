# Xbox 360 Wired Controller — Headset (Interface 1) Packet Format

Status: **PARTIALLY CHARACTERIZED**. No public byte-level dump of the live audio
stream of a *wired* Xbox 360 controller exists. This document separates what is
KNOWN (from protocol notes + descriptor dumps) from UNKNOWN (must be confirmed
experimentally). The diagnostic driver in `src/usb/usbh/x360audio/` is the tool
used to fill the UNKNOWN rows with a real controller.

References:

- [xboxdrv PROTOCOL](https://github.com/xboxdrv/xboxdrv/blob/master/PROTOCOL)
  (wired controller sections; includes experimental headset audio notes).
- [Parts Not Included — Understanding the Xbox 360 wired controller's USB data](https://partsnotincluded.com/understanding-the-xbox-360-wired-controllers-usb-data/)
- Windows kernel driver (xusb22.sys et al.) behaviour reported in xboxdrv notes.

---

## 1. Interface and endpoints (KNOWN)

Interface 1 of the wired controller `045e:028e`:

| Field | Value |
|---|---|
| bInterfaceClass | 0xFF (vendor specific) |
| bInterfaceSubClass | 0x5D |
| bInterfaceProtocol | 0x03 |
| bNumEndpoints | 4 |
| extra class descriptor | `1B 21 ...` (27 bytes, vendor 0x21) |

Endpoint map (physical addresses, per Parts Not Included + real descriptor dumps):

| Addr | Dir | Type | Size | Interval | xboxdrv name | Function |
|---|---|---|---|---|---|---|
| 0x82 | IN | interrupt | 32 B | 2 ms | "Endpoint 3" | **Microphone** (controller → host) |
| 0x02 | OUT | interrupt | 32 B | 4 ms | "Endpoint 4" | **Headphone/phone** (host → controller) |
| 0x83 | IN | interrupt | 32 B | 64 ms | "Endpoint 5" | **Status** (TO VERIFY) |
| 0x03 | OUT | interrupt | 32 B | 16 ms | "Endpoint 5" | **Config** (TO VERIFY) |

Note: xboxdrv numbers the headset endpoints 3/4/5 by their order in the
descriptor stream; the physical USB addresses are 0x82/0x02/0x83/0x03. Always
use the physical addresses.

---

## 2. Message framing across all interfaces (KNOWN)

Wired 360 messages are short messages with a common envelope on the *gamepad*
interface (if0):

```
byte 0   message/type
byte 1   payload length (excluding these 2 bytes)
bytes 2+ payload
```

On the *audio* interface (if1) the xboxdrv notes describe a similar
`[type][len][payload...]` shape for the status/config sub-messages, while the
audio endpoints (0x82/0x02) carry a pure ADPCM code stream with **no**
per-packet header (i.e. 32 raw code bytes — TO VERIFY on hardware).

---

## 3. Status / init messages (KNOWN, from xboxdrv PROTOCOL)

On first connect the controller sends on if0 in:

| Message | Meaning |
|---|---|
| `01 03 0e` | current LED status |
| `02 03 00` | UNKNOWN — "headset connection status or volume" |
| `03 03 03` | rumble status (0x03 = default enabled) |
| `08 03 00` | headset connection status — source comment: `0x00` no headset, `0x02` headset (conflicts with plug/unplug log below) |
| `00 14 ...` | gamepad data (20 bytes) |

Headset plug / unplug events observed (verbatim from xboxdrv log):

```
Plugging in  a headset: len: 3 data: 0x08 0x03 0x00
Pulling out  a headset: len: 3 data: 0x08 0x03 0x02
```

> CONFLICT: the source comment annotating `08 03 00` says "0x00 no headset,
> 0x02 headset", but the plug/unplug log lists `08 03 00` on plug-in and
> `08 03 02` on pull-out. Both quotes are reproduced verbatim. Resolve on
> hardware by watching if0-IN while plugging a known headset.

These `08`-type messages travel on the *gamepad* IN endpoint (if0, 0x81), not
on the audio interface. The incoming-user firmware's xinput host driver
currently drops them (`new_pad_data` is only set for type-0x14 gamepad frames),
so the headset-connect signal must be surfaced through a separate hook.

---

## 4. Headset init sequence (KNOWN, from xboxdrv PROTOCOL — TO VERIFY)

Observed on the controller when a headset is plugged in (xboxdrv logs these on
interface 1, status/config endpoint — physical 0x03 OUT / 0x83 IN):

```
len: 4 data: 00 04 01 00
len: 3 data: 01 03 00
len: 3 data: 02 03 0f
len: 5 data: 03 05 ff 00 00
len: 3 data: 04 03 ff
len: 3 data: 05 03 00
```

Pulling the headset out:

```
len: 4 data: 00 04 00 00
```

These are *observed frames*, not necessarily commands the host must send.
Whether the host must replay them (or which subset) to enable the microphone is
UNKNOWN — xboxdrv does not send them, it only logs them.

---

## 5. Config/status register commands (KNOWN, from xboxdrv PROTOCOL — TO VERIFY)

"Only works with the headset plugged in; settings seem permanent — handle with care."

| Command (if1 config OUT) | Reply (observed) | Effect |
|---|---|---|
| `00 03 0N`, N ∈ [0,1] | `05 03 0N` | "sets something to N" |
| `01 03 0N`, N ∈ [0,1] | `01 03 0N` | "sets something to N" |
| `02 03 0N`, N ∈ [0,f] | `02 03 0N` | "seems to be volume control" |
| `03 05 ??` | `03 05 ff 00 00` | no-op / reply echo |
| `04 03 ??` | `04 03 ff` | no-op / reply echo |
| `04 04 00 00` | status reply | triggers a status report |

---

## 6. Audio transport (KNOWN — codec, rates; TO VERIFY — live packet bytes)

From xboxdrv experimental headset notes (wired controller `--headset*`):

| Property | Value |
|---|---|
| Transfer size | 32-byte interrupt IN/OUT both directions. 16-byte OUT is accepted by the device but produces *silence* — always use 32 bytes. |
| Codec | 4-bit G.721 / G.726-style ADPCM (Sun `g72x` reference: `external/g72x/`) |
| Nibble packing | right-packed = **low nibble first, then high** (nodecoded `-R`). The alternate left-pack is understandable but worse. |
| Codec state | continuous across USB packets (no per-packet reset) |
| Capture (mic, 0x82 IN) | effective sample rate **16 kHz** (≈8192 bytes/s of codes ≈ 32 B × ~256 Hz) |
| Render (phone, 0x02 OUT) | effective sample rate **8 kHz** (64 codes/packet ⇒ ~125 packets/s) |

Rate asymmetry matters: a raw mic dump (16 kHz codes) replayed on the phone
path (8 kHz) plays at half speed. A straight dump→play loop is therefore the
wrong speed without resampling.

---

## 7. UNKNOWN — to be measured with the diagnostic layer

1. **Do the 0x82 / 0x02 audio endpoints stream when no headset is connected?**
   (idle frames vs silence). Expected: the mic EP either idles or emits a
   repeated status/zero frame — must be observed.
2. **Exact byte layout of a live 0x82 mic frame**: pure 32 ADPCM codes (implied
   by xboxdrv `--headset-dump`) vs any leading header/sequence bytes.
3. **Exact byte layout of the 0x02 phone frame** expected by the controller.
4. **Role of 0x83 IN / 0x03 OUT** — confirm they carry the status/config
   frames of sections 4–5 (they may instead carry something else, e.g. audio
   at extended intervals). Do NOT assume audio on these.
5. **Whether the status `08 03 00/02` frames also appear on 0x83 IN**, and
   which nibble-packed ADPCM variant the controller's mic expects if it emits
   frames with more codes than expected.
6. **Headset mute / volume** — whether `02 03 0N` (section 5) really is volume
   and whether a mute submessage exists.

Measurement plan (diagnostic mode, driver in `src/usb/usbh/x360audio/`):

1. Enumerate + open 0x82/0x02 (and 0x83/0x03 for logging).
2. Queue continuous IN on 0x82 from config; log first N frames verbatim, then
   rate-limit to one frame/sec + counters.
3. Send the section-5 config/volume probes one at a time and log 0x83 replies.
4. Record bytes with headset out vs plugged in; classify frames.
5. (Later) decode 0x82 payload as G.726 right-packed 16 kHz to validate codec
   hypothesis; encode 0x02 at 8 kHz and confirm headset produces sound.

### Runtime probe commands (CDC, `usb2usb` host builds)

On a host build with a CDC port (e.g. `make usb2usb_rp2350usba`) the serial
console accepts these commands; responses use the `x360audio:` prefix:

| Command | Action |
|---|---|
| `X360?` | active device addr + mic/status frame counters + first 4 bytes of last mic frame |
| `X360CFG=<hex>` | queue config bytes for the 0x03 OUT endpoint (`08 03 00` or `080300`) |
| `X360PHONE=<hex>` | send one 32-byte phone frame on 0x02 (rejected unless exactly 32 bytes) |
| `X360INIT` | replay the section-4 headset-attach sequence on 0x03 as a probe |

Probe procedure: run `X360?`, confirm `mic=`/`status=` are incrementing, then
plug/unplug the headset and watch those counters plus the if0 traffic change;
finally run `X360INIT` and `X360?` again to see whether the controller replies
on 0x83.