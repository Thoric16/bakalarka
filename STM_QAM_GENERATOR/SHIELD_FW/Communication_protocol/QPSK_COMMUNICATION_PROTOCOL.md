# QPSK communication protocol proposal

This document proposes a small, deterministic protocol for testing two Nucleo
boards connected as follows:

```text
Nucleo A DAC I/Q  ─────►  Nucleo B ADC I/Q
Nucleo B DAC I/Q  ◄─────  Nucleo A ADC I/Q
```

The first goal is reliable point-to-point QPSK communication. The protocol is
intentionally small enough to implement on an STM32 without an operating
system, while still providing unambiguous frame detection, symbol-clock
recovery, sequence checking, and error reporting.

This is a proposed project-specific format, not an attempt to implement a
complete radio standard.

## 1. Terminology and assumptions

- A **symbol** is one QPSK constellation point and carries two bits.
- A **frame** is one complete transmission, including synchronization symbols,
  header, payload, and CRC.
- `TX` and `RX` refer to the local board. Both boards can transmit and receive.
- The ADC observes the I and Q voltages. The current ADC test firmware reports
  these as calibrated signed millivolts on a 12-bit ADC.
- All multi-byte integer fields use **little-endian** byte order.
- The initial implementation should use a fixed symbol rate and a fixed number
  of ADC samples per symbol. These values can later be negotiated.

Recommended initial values:

| Parameter | Initial value |
| --- | ---: |
| ADC resolution | 12 bit |
| ADC sample rate | Fixed by the timer/DMA configuration |
| Samples per symbol | 8 or 16 |
| Modulation | QPSK, Gray coded |
| Payload limit | 128 bytes |
| CRC | CRC-16/CCITT-FALSE |
| Link direction | Half-duplex, one frame at a time |
| Frame timeout | 100 ms initially |

The symbol rate must be chosen so that the DAC update and ADC sampling timers
have sufficient margin. It should be a timer-derived value rather than a
software `delay()` loop.

## 2. QPSK constellation and bit mapping

Use Gray coding so that a small phase error normally changes only one bit:

| Dibits | I level | Q level | Nominal phase |
| --- | ---: | ---: | ---: |
| `00` | `+A` | `+A` | 45° |
| `01` | `-A` | `+A` | 135° |
| `11` | `-A` | `-A` | 225° |
| `10` | `+A` | `-A` | 315° |

`A` is the calibrated symbol amplitude. Keep the output centered around the
ADC midpoint and leave headroom for offsets and noise. The receiver should
normalize I and Q before slicing, for example by estimating the average
magnitude from the synchronization preamble.

Bits are consumed most-significant dibit first within each byte:

```text
byte 0x1B = 00011011
          = 00 01 10 11
          = symbols 00, 01, 10, 11
```

The receiver must document and use one phase convention consistently. If the
physical wiring inverts I or Q, handle that with a configurable
`constellation_transform` rather than silently changing the bit mapping.

## 3. Frame structure on the analog link

Every transmission has this structure:

```text
AGC_PREFIX | SYNC_WORD | TIMING_TRAIN | FRAME_HEADER | PAYLOAD | CRC | GUARD
```

### 3.1 AGC prefix

Transmit 32 repeated symbols of `00` (the `+A,+A` constellation point).

The receiver uses this section to:

- detect that a transmission is present;
- estimate I/Q offset;
- estimate signal amplitude;
- initialize automatic gain or normalization.

The receiver should not decode these symbols as payload.

### 3.2 Sync word

Transmit the following 16 dibits, where each dibit is one QPSK symbol:

```text
00 11 01 10  10 01 11 00  01 10 00 11  11 00 10 01
```

This sequence has frequent transitions and is unlikely to occur as a long
constant section. The receiver searches for it after amplitude detection and
uses the best correlation peak as the frame start.

### 3.3 Timing training

Transmit 64 symbols alternating between `00` and `11`:

```text
00 11 00 11 00 11 ...
```

This provides regular transitions for symbol timing recovery. The receiver
should estimate the symbol period and sampling phase from these transitions,
then sample near the center of each following symbol. A simple first version
can use:

1. a known timer-derived nominal period;
2. correlation against the timing train;
3. a small early/late correction after every symbol.

The recovered clock is local to the receiving Nucleo. The analog link does not
provide a shared electrical clock. If a common clock is required instead of
clock recovery, connect a separate timer/clock signal and treat it as a
hardware interface outside this protocol.

### 3.4 Header, payload, and CRC

After the timing train, transmit the byte-oriented fields below. Each byte is
converted to four QPSK symbols as described in Section 2.

```text
FRAME_HEADER | PAYLOAD | CRC16
```

The header is always 12 bytes:

| Offset | Size | Field | Description |
| ---: | ---: | --- | --- |
| 0 | 1 | `version` | Protocol version, initially `0x01` |
| 1 | 1 | `type` | Message type from Section 4 |
| 2 | 1 | `flags` | Direction and optional feature flags |
| 3 | 1 | `source_id` | Sender ID, normally `0x01` or `0x02` |
| 4 | 1 | `destination_id` | Receiver ID, or `0xFF` for broadcast |
| 5 | 1 | `reserved` | Transmit as `0x00`; reject non-zero in v1 |
| 6 | 2 | `sequence` | Little-endian frame sequence number |
| 8 | 2 | `payload_length` | Little-endian byte count, 0 to 128 |
| 10 | 2 | `symbol_rate` | Symbols/second, little-endian |

`CRC16` covers the complete 12-byte header followed by the payload. It does
not cover the AGC prefix, sync word, or timing train.

CRC-16/CCITT-FALSE parameters:

```text
polynomial = 0x1021
initial    = 0xFFFF
reflect in = false
reflect out= false
xor out    = 0x0000
```

Transmit the CRC low byte first, followed by the high byte.

### 3.5 Guard interval

Append eight zero-amplitude symbols after the CRC. The transmitter then
returns to receive mode (or waits for the configured turnaround delay). The
guard prevents the receiver from interpreting DAC shutdown transients as
additional symbols.

## 4. Message types

The `type` field identifies the payload format:

| Value | Name | Payload |
| ---: | --- | --- |
| `0x01` | `LINK_PING` | 4-byte nonce |
| `0x02` | `LINK_PONG` | 4-byte nonce copied from `LINK_PING` |
| `0x03` | `CLOCK_TRAIN` | 4-byte requested symbol count, normally `64` |
| `0x04` | `DATA` | Application bytes |
| `0x05` | `STATUS` | 1-byte status code plus optional detail bytes |
| `0x06` | `NACK` | 1-byte error code and 2-byte rejected sequence |
| `0x07` | `CONFIG` | Versioned link parameters; use only after basic link works |

Suggested status/error codes:

| Code | Meaning |
| ---: | --- |
| `0x00` | OK |
| `0x01` | Unsupported protocol version |
| `0x02` | Invalid message type |
| `0x03` | Payload too large |
| `0x04` | CRC failure |
| `0x05` | Synchronization timeout |
| `0x06` | Invalid destination |
| `0x07` | Sequence number error |

An invalid CRC cannot be trusted to contain a valid sequence number. Therefore,
the receiver should normally discard a CRC-invalid frame silently and
increment a local error counter. It may send `NACK(CRC_FAILURE, 0xFFFF)` only
when a reliable diagnostic channel is more important than avoiding extra
traffic.

## 5. Flags and sequence handling

Define the `flags` byte as:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `ACK_REQUEST` | Receiver should answer with `STATUS(OK)` |
| 1 | `ACK_RESPONSE` | This frame is an acknowledgement |
| 2 | `RETRANSMISSION` | Same sequence number is being sent again |
| 3 | `CLOCK_TRAIN_PRESENT` | A non-default timing train is present |
| 4-7 | reserved | Transmit as zero; ignore when receiving in v1 |

The sender increments `sequence` for each new frame and keeps the same value
when retransmitting. The receiver can use `(source_id, sequence)` to detect a
duplicate. For the first test, acknowledgements are optional and there is no
sliding window: send one frame, wait for its response or timeout, then send the
next frame.

## 6. Receiver state machine

Implement the receiver as an explicit state machine:

```text
IDLE
  -> AGC_DETECTED
  -> SYNC_FOUND
  -> TIMING_LOCKED
  -> HEADER_RECEIVED
  -> PAYLOAD_RECEIVED
  -> CRC_CHECK
  -> DELIVER or DISCARD
```

Required transitions:

- `IDLE -> AGC_DETECTED`: signal magnitude exceeds the configured threshold.
- `AGC_DETECTED -> SYNC_FOUND`: sync correlation exceeds the threshold.
- `SYNC_FOUND -> TIMING_LOCKED`: timing train gives a valid symbol period and
  sampling phase.
- Any state -> `IDLE`: timeout, impossible field value, or signal loss.
- `CRC_CHECK -> DELIVER`: CRC matches, destination matches, and length is valid.
- `CRC_CHECK -> DISCARD`: CRC, version, destination, or length is invalid.

Keep counters for at least `sync_failures`, `crc_failures`,
`frames_received`, `frames_transmitted`, and `duplicate_frames`. Expose these
counters through a diagnostic `STATUS` response or a serial debug command.

## 7. Clock and synchronization procedure

There are two different clocks to keep separate:

1. **ADC/DAC sample clock:** generated by a local hardware timer on each board.
2. **QPSK symbol clock:** recovered by the receiver from the known timing train.

Recommended implementation:

1. Configure a timer-triggered ADC with DMA.
2. Configure a timer-triggered DAC update from a symbol/sample buffer.
3. Use a nominal `samples_per_symbol` value of 8 or 16.
4. Detect the AGC prefix and estimate I/Q offset and amplitude.
5. Correlate with the sync word.
6. Use the alternating timing train to estimate symbol period and phase.
7. Sample each symbol at the estimated center.
8. Apply a small early/late correction to compensate for oscillator mismatch.
9. Decode the header and use its `symbol_rate` as a consistency check.

Do not attempt to synchronize the boards by comparing timestamps printed over
the UART. UART is a diagnostic/control channel and is not phase-aligned with
the analog samples.

If the two Nucleos must operate continuously without frame gaps, add a
periodic `CLOCK_TRAIN` frame or a pilot section to every long `DATA` transfer.
For the initial test, one synchronization sequence per frame is simpler and
more observable.

## 8. Initial test sequence

### Test 1: constellation and analog path

1. Transmit each of the four dibits repeatedly.
2. Plot received I/Q values using the existing ADC monitor.
3. Verify the four clusters and check whether I, Q, or both axes are inverted.
4. Select the corresponding `constellation_transform` configuration.

### Test 2: synchronization only

1. Transmit AGC prefix, sync word, and timing train.
2. Do not transmit a payload yet.
3. Log correlation position, estimated amplitude, samples/symbol, and timing
   error.
4. Repeat with both directions: A to B and B to A.

### Test 3: fixed known frame

Use this payload for a deterministic first frame:

```text
type            = LINK_PING (0x01)
source_id       = 0x01
destination_id  = 0x02
sequence        = 0x0001
payload         = 78 56 34 12
```

The receiver should report a valid CRC and reply with `LINK_PONG`, copying the
four nonce bytes. Verify the same test in the reverse direction.

### Test 4: error handling

Verify that the receiver:

- rejects a frame with one payload bit changed;
- rejects a payload length greater than 128;
- ignores a frame addressed to the other board;
- detects a repeated sequence number as a duplicate;
- returns to `IDLE` after a truncated frame;
- recovers after a deliberately missed frame.

## 9. UART diagnostics

Keep the binary QPSK stream separate from diagnostic UART output. A compact
human-readable line format is sufficient:

```text
RX OK type=0x04 src=0x01 seq=42 len=16 snr=18.2 timing_err=0.04
RX ERR reason=CRC seq=42 crc_rx=0x1234 crc_calc=0x5678
LINK stats rx=100 tx=100 crc=0 sync=0 dup=2
```

This keeps the existing ADC plotting workflow useful for analog bring-up,
while the new protocol implementation can provide frame-level diagnostics.
Do not emit these text lines on the analog QPSK path.

## 10. Versioning and future extensions

Version `0x01` is intentionally conservative. Future changes should use a new
protocol version or an explicitly negotiated `CONFIG` message. Do not reuse a
field with a different meaning within the same version.

Possible later extensions are:

- configurable symbol rate and samples per symbol;
- pilot symbols inserted into long payloads;
- forward-error correction;
- interleaving;
- full-duplex operation;
- a shared hardware reference clock;
- authenticated frames if the link is used outside laboratory testing.

