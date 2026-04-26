# Master-Slave Multi-Track Sync Architecture

## Table of Contents

1. [Goals and Non-Goals](#1-goals-and-non-goals)
2. [Plugin Modes](#2-plugin-modes)
3. [Inter-Plugin Communication: Shared Memory IPC](#3-inter-plugin-communication-shared-memory-ipc)
4. [Shared Memory Layout](#4-shared-memory-layout)
5. [Lock-Free Synchronisation](#5-lock-free-synchronisation)
6. [Master Plugin - Responsibilities](#6-master-plugin--responsibilities)
7. [Slave Plugin - Responsibilities](#7-slave-plugin--responsibilities)
8. [Fusion Policy (per slave)](#8-fusion-policy-per-slave)
9. [Disconnect and Hold Behaviour](#9-disconnect-and-hold-behaviour)
10. [LTC Channel Selection](#10-ltc-channel-selection)
11. [Session Configuration](#11-session-configuration)
12. [Master UI](#12-master-ui)
13. [Slave UI](#13-slave-ui)
14. [Platform Implementation Notes](#14-platform-implementation-notes)
15. [Data Flow Diagram](#15-data-flow-diagram)
16. [Design Decisions and Rationale](#16-design-decisions-and-rationale)

---

## 1. Goals and Non-Goals

**Goals:**
- Synchronise up to 8 independent audio tracks to one reference track, DAW-independently.
- Impose no audio routing requirements between tracks - plugins communicate only via OS shared
  memory.
- Keep CPU overhead proportional to the number of active slaves, with each slave doing its own
  analysis.
- Be robust to LTC degradation via the existing audio-domain fallback (NCC on novelty curves).
- Visually signal degraded or disconnected state so the operator knows when to act.

**Not goals:**
- Support for more than 8 slaves (fixed for v1; revisit if needed).
- Video synchronisation.
- Networking across machines (shared memory is local only).
- Backward compatibility with the stereo PoC. The PoC is replaced in full.

---

## 2. Plugin Modes

The plugin ships as a single VST3 binary. A **Mode** control in the UI selects one of two roles:

| Mode   | Where to insert | What it does |
|--------|-----------------|--------------|
| Master | Reference track | Decodes LTC, extracts reference novelty curve, writes both to shared memory. Renders a slot-status dashboard. Does NOT delay its output. |
| Slave  | Each track to delay | Decodes its own LTC, extracts its own novelty curve, reads the reference data from shared memory, computes and applies delay. |

There is no Standalone mode. A session always has exactly one Master and one to eight Slaves.

---

## 3. Inter-Plugin Communication: Shared Memory IPC

All inter-plugin data exchange uses **OS-level named shared memory**:

- **Linux / macOS:** `shm_open` + `mmap` (POSIX, `librt`)
- **Windows:** `CreateFileMapping` + `MapViewOfFile`

The shared memory region is named `"AUTOSYNC2_{groupName}"`, where `groupName` is a short
alphanumeric string (max 16 chars) configured identically in the Master and all Slaves of a
session. Different sessions on the same machine use different group names. The `2` suffix is a
layout-version guard: it prevents a new plugin build from attaching to a segment written by an
older build whose `MasterSlot` had a different field layout (the nov_ref array was smaller in
earlier versions).

**Why shared memory instead of alternatives:**

| Option | Rejected because |
|--------|-----------------|
| Multi-channel sidechain routing to master | DAW-dependent; multi-channel sidechain support is inconsistent across hosts. |
| MIDI routing for delay commands | Requires in-DAW MIDI routing; limited bandwidth; no path for analysis data. |
| Local sockets / named pipes | More complex setup; introduces network-stack latency. |
| Static process-global map | Fails if DAW runs plugins in separate processes (sandboxed mode). |

Shared memory is DAW-independent, crosses process boundaries, and carries only small structs -
no audio buffer transfer is ever required.

---

## 4. Shared Memory Layout

```cpp
// Number of hops in the master novelty ring (10 ms/hop → 20 s of history).
// Must stay in sync with AudioFallbackState::windowFrames (set in init()).
static constexpr int MASTER_NOV_REF_SIZE = 2000;

// Written by master; read by all slaves.
struct MasterSlot
{
    std::atomic<uint32_t> writeSeq { 0 };  // seqcount: odd = write in progress

    bool    valid           = false;   // false until master has produced ≥1 full window
    uint8_t ltc_state       = 0;       // 0=FAIL, 1=SUSPECT, 2=VALID
    uint8_t locked          = 0;       // temporal-coherence gate state: 0=unlocked, 1=locked

    int64_t tc_ref_ms         = 0;     // reference timecode in milliseconds
    int64_t ref_decode_sample = 0;     // abs stream sample pos of last master LTC decode
    int64_t nov_anchor_sample = 0;     // DAW-timeline sample pos when novelty was last written;
                                       // used by slaves to correct for async SM read latency

    float   Q_ref             = 0.0f;

    int     nov_writePos      = 0;     // circular buffer head (next write index)
    int     nov_framesFilled  = 0;     // frames populated so far, saturates at MASTER_NOV_REF_SIZE

    float   nov_ref[MASTER_NOV_REF_SIZE] = {};  // reference log-energy novelty ring (~8 KB)
};
// sizeof(MasterSlot) ≈ 8100 bytes (dominated by nov_ref[2000] × 4 bytes = 8 KB)

// Written by one slave; read by master (for dashboard).
struct SlaveSlot
{
    std::atomic<uint32_t> writeSeq { 0 };

    bool    connected       = false;   // true while the slot is actively being written
    bool    holding         = false;   // delay frozen because master data is stale
    uint8_t ltc_state       = 0;
    uint8_t fusion_src      = 0;       // 0=None, 1=LTC, 2=Audio

    char    label[32]       = {};      // user-assigned label, null-terminated

    int64_t tc_self_ms      = 0;
    float   Q_self          = 0.0f;
    float   estimated_fps   = 0.0f;

    double  dt_ltc_ms       = 0.0;    // Δt from LTC path
    double  dt_aud_ms       = 0.0;    // Δt from audio NCC path
    double  conf_aud        = 0.0;
    double  active_delay_ms = 0.0;    // delay currently applied
};
// sizeof(SlaveSlot) ≈ 112 bytes

struct SharedGroup
{
    MasterSlot master;
    SlaveSlot  slaves[8];
};
// sizeof(SharedGroup) ≈ 9000 bytes (well within one OS page of 4096 × 3)
```

The shared memory region is exactly `sizeof(SharedGroup)` bytes, allocated once by the first
plugin instance that opens it (master or slave, whichever starts first), and released when all
instances close their handles.

---

## 5. Lock-Free Synchronisation

Both `MasterSlot` and `SlaveSlot` use a **seqcount** pattern:

**Writer (audio thread):**
```cpp
slot.writeSeq.fetch_add(1, std::memory_order_release);  // make seq odd
// ... write fields ...
slot.writeSeq.fetch_add(1, std::memory_order_release);  // make seq even
```

**Reader (audio thread of another instance):**
```cpp
uint32_t seq1, seq2;
do {
    seq1 = slot.writeSeq.load(std::memory_order_acquire);
    if (seq1 & 1) continue;       // write in progress, spin
    // ... read fields into local copies ...
    seq2 = slot.writeSeq.load(std::memory_order_acquire);
} while (seq1 != seq2);
```

This is wait-free for the writer and briefly spinning for the reader only when a write is in
progress. Writes happen at window boundaries (~every 200–500 ms), making collisions rare.
No mutex, no priority inversion, no allocation on the audio thread.

---

## 6. Master Plugin - Responsibilities

### Audio thread (`processBlock`)

Per sample:
- Decode LTC from the designated LTC channel (left or right of master's stereo input).
- Accumulate energy for the novelty hop (same `AudioFallbackState` hop logic as current).

At each novelty hop boundary (~10 ms):
- Compute hop novelty, advance `nov_ref` circular buffer.
- Increment `nov_framesFilled` until saturated at 200.

At each LTC quality window boundary (~500 ms):
- Call `computeAndResetWindow()` → updates `Q_ref`, `ltc_state`.

At each NCC refresh interval (~200 ms):
- Write to `MasterSlot` via seqcount:
  - `tc_ref_ms`, `ref_decode_sample`, `Q_ref`, `ltc_state`, `valid`
  - Full `nov_ref[2000]` copy + `nov_writePos`, `nov_framesFilled`, `nov_anchor_sample`

### GUI thread (timer, ~100 ms)
- Read all 8 `SlaveSlot` entries from shared memory.
- Render slot table (see §12).

### What master does NOT do
- Apply any delay to its own output. The master track is the reference; it passes audio through unchanged.
- Command delay values to slaves. Slaves compute and apply their own delays.

---

## 7. Slave Plugin - Responsibilities

### Configuration (set by user, stored in plugin state)
- **Group name**: must match master's group name exactly.
- **Slot ID**: 1–8, unique within the group.
- **LTC channel**: Left or Right (which channel of the slave's stereo input carries LTC).
- **Label**: short human-readable name shown in master UI (e.g. `"field-rec-A"`).

### Audio thread (`processBlock`)

Per sample:
- Decode LTC from the designated LTC channel.
- Pass both channels through the delay engine (delay applied to the whole output bus).
- Accumulate energy for novelty hop.

At each novelty hop boundary (~10 ms):
- Compute `nov_self` hop novelty, advance its circular buffer.

At each LTC quality window boundary (~500 ms):
- Call `computeAndResetWindow()` → updates `Q_self`, `ltc_state_self`.

At each diagnostic interval (~100 ms):
- Read `MasterSlot` from shared memory (seqcount read).
- If master is valid, `master.ltc_state >= SUSPECT`, and **both decoders have the
  temporal-coherence gate engaged** (`master.locked && slave_chnl.locked`):
  - Compute `dt_ltc_ms` using the sample-accurate formula:
    ```
    dt_ltc_ms = (ref_decode_sample_slave − ref_decode_sample_master) / sr × 1000
                − (tc_self_ms − tc_ref_ms)
    ```
    The subtracted term cancels LTC frame-boundary quantisation (±1 frame = ±40 ms at 25 fps),
    giving sub-millisecond precision regardless of when the two 0.1 s update ticks fire.
    Falls back to `tc_ref_ms − tc_self_ms` until either side hasn't decoded a frame yet.
  - The `bothLocked` gate prevents garbage frames (speech-derived BCD-valid collisions that
    passed through the coherence gate before re-locking) from corrupting `d_ms` during
    LTC fade or recovery.
- Novelty ring from SM is linearised into `masterNoveltyRef` when
  `nov_framesFilled >= WIDE_WIN` (4 s minimum before NCC can run).

At each NCC refresh interval (~200 ms, triggered from inside `pushAudioAnalysisSample`):
- If `activityGate` is open: run `estimateAudioFallbackOffset()` → `dt_aud_ms`, `conf_aud`.
- Run `fuseLtcAndAudioFallback()` → `active_delay_ms`, `fusion_src`.
- Update α-β tracker and delay engine with `active_delay_ms`.
- Write own status to `SlaveSlot[slotId-1]` via seqcount.

### NCC runs locally in the slave

The slave receives only the reference novelty curve (800 bytes) from shared memory. It runs
`NCC(nov_ref, nov_self)` using its local `AudioFallbackState`, with `nov_ref` used in place of
the old `novelty2` buffer. No audio samples ever leave the slave process.

---

## 8. Fusion Policy (per slave)

Each slave runs its own fusion logic independently. The policy mirrors and extends the existing
single-track fusion from `audio-sync-implementation.md §6`.

```
inputs:
  ltc_state_self   -- FAIL / SUSPECT / VALID  (slave's own LTC)
  ltc_state_ref    -- FAIL / SUSPECT / VALID  (master's LTC, read from MasterSlot)
  dt_ltc_ms        -- LTC-derived offset (valid only if both states are not FAIL)
  conf_aud         -- audio NCC confidence [0, 1]
  dt_aud_ms        -- NCC-derived offset
  aud_valid        -- stableCount >= 3 and conf_aud > 0.3
  master_valid     -- MasterSlot.valid is true and not stale (age < 2s)

policy:
  if master_valid
    and ltc_state_self == VALID
    and ltc_state_ref  == VALID:
      source = LTC
      delay  = dt_ltc_ms

  else if aud_valid and conf_aud > 0.4:
      source = Audio
      delay  = dt_aud_ms

  else:
      source = None
      delay  = hold (see §9)
```

**Key differences from the PoC fusion:**

- There are now two LTC states to check: the slave's own and the master's. If the master's LTC
  is FAIL, the LTC path is unavailable regardless of the slave's LTC quality.
- The audio fallback (`source = Audio`) fires whenever either LTC state degrades below VALID
  (not only on FAIL), provided `conf_aud > 0.4`. This matches the intent of the original
  `SUSPECT` handling in the existing fusion spec.
- `source = None` → hold behaviour (§9), not zero.

---

## 9. Disconnect and Hold Behaviour

### Master goes stale

The slave tracks the age of the last successfully read `MasterSlot`. If no valid master write
has been received for more than 2 seconds:

- `master_valid = false`
- Fusion falls through to `source = None`
- The delay engine holds the last committed `active_delay_ms` - it does not zero out.
- `SlaveSlot.holding = true` is written to shared memory.

### Visual indication

In the slave UI, when `holding = true`:
- The delay readout changes colour to amber.
- A `[HOLD]` label appears next to the delay value.
- The connection indicator changes from green to amber.

In the master UI slot table:
- The slave's row is rendered in amber with a `[HOLD]` tag.

When the master reconnects and `master_valid` returns to `true`, the slave resumes normal
fusion, clears `holding`, and the UI returns to its normal state.

### Slave disconnects (as seen from master)

Each `SlaveSlot` has a `connected` flag. A slave sets `connected = true` on each write and has
a background timer that sets `connected = false` if `processBlock` has not been called for more
than 2 seconds (e.g. track muted or plugin bypassed). The master UI renders disconnected slots
in grey with a `[–]` tag.

---

## 10. LTC Channel Selection

Each plugin instance (master and slave) has a **LTC Channel** selector: `Left` or `Right`.

This controls which channel of the stereo input is fed to the LTC decoder (`processTimeCode`).
The delay engine always operates on the full stereo output of the slave - both channels are
delayed by the same `active_delay_ms`. The LTC channel is only used for decoding; it is delayed
along with everything else.

If a track has LTC on a separate mono track routed in via a sidechain, that is out of scope for
v1. LTC must be on L or R of the plugin's main stereo input.

---

## 11. Session Configuration

### What the user configures (one-time, per session)

| Plugin | Setting | Notes |
|--------|---------|-------|
| Master | Mode = Master | |
| Master | Group name | e.g. `"shoot1"` |
| Master | LTC Channel | L or R |
| Slave  | Mode = Slave | |
| Slave  | Group name | must match master |
| Slave  | Slot ID | 1–8, unique |
| Slave  | LTC Channel | L or R |
| Slave  | Label | e.g. `"boom-1"` |

All settings are saved via `getStateInformation` / `setStateInformation` (JUCE XML state).
They survive DAW save/reload without reconfiguration.

### Slot ID assignment

Slot IDs are assigned manually by the user. They are stable across sessions as long as the
plugin state is saved. The master UI slot table displays slots in order 1–8; empty (never
connected) slots are shown greyed out. There is no auto-assignment or discovery protocol -
the user decides which recorder gets which slot, exactly as they would assign bus numbers on a
hardware desk.

---

## 12. Master UI

The master UI replaces the current stereo channel pair display with a slot dashboard.

```
┌─ AUTOSYNC Master ── group: shoot1 ──────────────────────────────────────┐
│  LTC In: 01:02:34:12   Q=0.91  VALID   FPS=25.0                         │
├──────────────────────────────────────────────────────────────────────────┤
│  Slot  Label         LTC       Q     Δt(LTC)   Δt(AUD)  conf  Src   St  │
│   1    field-rec-A   VALID   0.88   -80.0 ms   -79 ms  0.82  LTC   ✓   │
│   2    boom-1        FAIL    0.14      --       -81 ms  0.71  AUD   ✓   │
│   3    cam-b-audio   SUSPECT 0.54   -80.5 ms   -82 ms  0.61  AUD  [!]  │
│   4    –             –       –         –          –      –     –   [–]  │
│   5    –             –       –         –          –      –     –   [–]  │
│   6    –             –       –         –          –      –     –   [–]  │
│   7    –             –       –         –          –      –     –   [–]  │
│   8    –             –       –         –          –      –     –   [–]  │
├──────────────────────────────────────────────────────────────────────────┤
│  Status legend:  ✓ normal   [!] suspect/audio-fallback  [–] disconnected │
└──────────────────────────────────────────────────────────────────────────┘
```

Status column colour coding:
- Green `✓` - LTC VALID, source = LTC
- Amber `[!]` - fallback active or LTC SUSPECT
- Amber `[HOLD]` - slave is holding last delay (master was stale from slave's perspective; this
  should not appear in a healthy session but is shown if a slave reports `holding = true`)
- Grey `[–]` - slot never connected or `connected = false`

---

## 13. Slave UI

The slave UI is a simplified version of the current plugin editor, tuned to its single-track role.

```
┌─ AUTOSYNC Slave ── group: shoot1  slot: 2  [field-rec-A] ───────────────┐
│  LTC In:   01:02:34:18   Q=0.88   VALID   FPS=25.0                      │
│  Ref LTC:  01:02:34:12   Q=0.91   VALID   (from master)                 │
├──────────────────────────────────────────────────────────────────────────┤
│  Δt LTC:  -80.0 ms     Δt AUD:  -79 ms   conf: 0.82                    │
│  Active delay:  -80.0 ms   [LTC]    ● connected                         │
│                                                                          │
│  [Config: Mode ▼]  [LTC Ch: L ▼]  [Slot: 2 ▼]  [Group: shoot1      ]   │
└──────────────────────────────────────────────────────────────────────────┘
```

When holding:
- "Active delay" field turns amber: `  -80.0 ms   [HOLD]  ⚠ master lost`
- Connection indicator turns amber.

When master not yet seen (first startup before master writes):
- "Ref LTC" shows `--:--:--:--  (waiting for master)`
- Delay is not applied until master sends at least one valid window.

---

## 14. Platform Implementation Notes

### SharedGroupMemory wrapper

A new header `SharedGroupMemory.h` (~120 lines) wraps the OS primitives behind a simple interface:

```cpp
class SharedGroupMemory
{
public:
    // Opens or creates the named shared memory region.
    // All instances in a group call open(); the first one creates it.
    bool open(const std::string& groupName);
    void close();

    SharedGroup* get();   // returns pointer into the mapped region, nullptr if not open

private:
#if defined(_WIN32)
    HANDLE hMapping = nullptr;
#else
    int    fd = -1;
#endif
    SharedGroup* ptr = nullptr;
};
```

`open()` creates the segment if it does not exist, or attaches to the existing one. The OS
reference-counts the mapping; the last `close()` unmaps but does not destroy the segment (it
persists until the OS is rebooted or until explicitly unlinked). This is intentional: if the
master crashes and restarts, it re-opens the same segment without losing slave registrations.

On Linux/macOS the segment is created under `/dev/shm/AUTOSYNC2_{groupName}`.  
On Windows the segment name is `Local\AUTOSYNC2_{groupName}`.

### CMake changes

On Linux, link `rt` for `shm_open`:
```cmake
if (UNIX AND NOT APPLE)
    target_link_libraries(AudioSyncPlugin PRIVATE rt)
endif()
```

### Audio thread safety

`SharedGroupMemory::get()` returns a raw pointer. All reads and writes on the audio thread use
the seqcount pattern (§5). No allocation, no system calls, no locks on the audio thread after
`open()` is called in `prepareToPlay`.

`open()` and `close()` are called from `prepareToPlay` and `releaseResources` respectively -
never from the audio callback.

---

## 15. Data Flow Diagram

```
Reference Track                          Slave Track 1              Slave Track 2
────────────────                         ─────────────              ─────────────
[Master Plugin]                          [Slave, Slot=1]            [Slave, Slot=2]

per sample:                              per sample:                per sample:
  decode LTC → tc_ref                     decode LTC → tc_self       decode LTC → tc_self
  accumulate energy                       accumulate energy          accumulate energy
                                          apply delay (audio out)    apply delay (audio out)

every ~10 ms (novelty hop):              every ~10 ms:              every ~10 ms:
  update nov_ref[] circular buf           update nov_self[]          update nov_self[]

every ~500 ms (LTC window):              every ~500 ms:             every ~500 ms:
  update Q_ref, ltc_state                 update Q_self, ltc_state   update Q_self, ltc_state

every ~200 ms (NCC refresh):             every ~200 ms:             every ~200 ms:
  WRITE MasterSlot to SM:                 READ MasterSlot from SM    READ MasterSlot from SM
    tc_ref_ms, ref_decode_sample,         NCC(nov_ref, nov_self)     NCC(nov_ref, nov_self)
    Q_ref, ltc_state, nov_ref[]            → dt_aud, conf_aud         → dt_aud, conf_aud
                                          compute dt_ltc (†)         compute dt_ltc (†)
                                          run fusion → delay_ms      run fusion → delay_ms
                                          update delay engine        update delay engine
                                          WRITE SlaveSlot[0] to SM   WRITE SlaveSlot[1] to SM

every ~100 ms (GUI timer):
  READ all SlaveSlots from SM
  render slot table
```

---

## 16. Design Decisions and Rationale

| Decision | Rationale |
|----------|-----------|
| No audio routing between plugins | The only cross-track data needed is the reference novelty curve (800 bytes / 200ms). Routing audio is DAW-dependent and imposes latency. |
| Slaves compute their own delay | Avoids a master→slave command round-trip (~200 ms). Each slave has all the data it needs locally plus the reference data from SM. |
| Fixed 8-slot layout | Fixed-size struct in shared memory avoids dynamic allocation and layout negotiation. 8 tracks covers the vast majority of real multi-camera shoots. |
| seqcount, not mutex | Audio thread must never block. Mutex with PROCESS_SHARED attribute risks priority inversion under DAW real-time scheduling. seqcount spins only during the brief write window (~microseconds). |
| Hold on lost master, not zero | Zeroing the delay on master dropout causes a rebuild of the delay engine and a transient glitch. Holding the last value is sonically invisible and gives the operator time to notice and respond. |
| Manual slot IDs, no auto-discovery | Auto-discovery requires a handshake protocol. Manual IDs are set once per session and saved with plugin state. They are unambiguous and require no runtime negotiation. |
| Single plugin binary, mode switch | Simpler distribution. Both roles share the LTC decoder, AudioFallbackState, and SharedGroupMemory code; only the UI and the SM write/read direction differ. |
