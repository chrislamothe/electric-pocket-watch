# Electric Pocket Watch: Timekeeping Report and Rebuild Plan

Status as of 2026-11-23. Repository: `chrislamothe/electric-pocket-watch`, branch `main`, head `25d896d`
(watch firmware v0.15.0 clock layer; clock lab firmware v0.15.4).

This document is written to be handed to other tools. Each statement is labelled where it matters:
**[measured]** came from a device run, **[verified]** was checked against source code or vendor
documentation, **[inferred]** is a reasoned conclusion not yet proven, **[decision]** is agreed with the
owner, **[open]** is unresolved.

---

## 1. Requirements from the owner

1. Timekeeping is the most important function of the project. "It'd be a failure without it." [decision]
2. Battery life is the second priority; it must not be traded away silently for timekeeping. [decision]
3. **No added hardware.** The stock **ESP32-S3-Touch-AMOLED-1.75C is the main test platform** and must stay
   supported. Boards: 1.75C (no RTC chip), 1.75 / 1.75-B and 2.06 (PCF85063 RTC). [decision]
4. The timekeeping layer is to be rebuilt from the ground up, not patched. [decision]
5. The owner's specification ("Replacement Timekeeping Layer", 2026-11-21, revised with section 18
   "Calibrated Internal Clock Sleep") governs the design. Key points from it:
   - A clock service owns UTC; the libc clock is only a compatibility mirror.
   - Every published time comes from an accepted, identified source sample. No build-time or NVS
     timestamp is ever "present time".
   - Uncertainty is tracked and never shown as zero; unqualified accuracy is shown as degraded.
   - Only the service touches RTC hardware or applies UTC; network delivers measurements.
   - Sleep: prepare/resume handshake (not a lock held across sleep); each successful sleep invalidates
     the awake mapping.
   - RTC boards re-acquire from the RTC on wake; RTC writes only from a network/manual reference,
     verified by read-back and advancement.
   - 1.75C: software holdover model `R = g_a*A + g_s*S + b*N` (awake time, sleep time, number of
     sleeps), **fitted offline**, loaded as a qualified versioned record; **no online adaptive learner**
     in version one; **no silent scheduled Wi-Fi** to hide drift.
   - Pure model module with host tests; standalone clock firmware before UI integration; hardware
     acceptance per board. Proposed target: <= 2 s/day displayed error offline (to be agreed).
6. Observed symptom reported by the owner: **all boards lose about 15 minutes per day** (earlier also
   reported "about 12 minutes per day"; the 1.75-B was once reported at ~10 s/day before v0.13.1).
   Exact firmware versions and intervals for those observations are not recorded. [open]

## 2. Hardware and platform facts

| Item | Value | Status |
|---|---|---|
| SoC | ESP32-S3, ESP-IDF v5.5.5 | verified |
| ESP32 32 kHz crystal | Not fitted on these boards (GPIO15/16 used for I2C etc.) | verified (pin map) |
| Slow clock (sleep timebase) | `CONFIG_RTC_CLK_SRC_INT_8MD256` (RC_FAST / 256) in the watch build | verified |
| RTC chip | PCF85063 at I2C 0x51 on 1.75 / 1.75-B / 2.06; absent on 1.75C | verified |
| Awake timebase | 40 MHz crystal (esp_timer) | verified |

ESP-IDF v5.5.5 behaviour relevant to timekeeping [verified from source]:

- `components/esp_hw_support/sleep_modes.c`: before **every** light sleep the slow clock is
  re-calibrated over **`RTC_CLK_SRC_CAL_CYCLES` = 10 cycles**; after waking, esp_timer is advanced by
  slow-clock ticks x that calibration, then `esp_set_time_from_rtc()` updates libc time. esp_timer and
  libc time are therefore **not independent** of the sleep clock.
- `CONFIG_RTC_CLK_CAL_CYCLES` applies to boot calibration, **not** to the per-sleep calibration.
- `components/lwip/apps/sntp/sntp.c`: `sntp_sync_time()` is a **weak** function whose default calls
  `settimeofday()` and then the notification callback that `esp_netif_sntp_sync_wait()` waits on.
  Overriding it lets SNTP deliver a measurement without setting the clock (the wait must then be
  replaced).

## 3. History of the timekeeping code (what was tried)

| Version | Commit | Change | Outcome |
|---|---|---|---|
| <= v0.12 | - | libc time; RTC (1.75 boards) read only at cold boot; SNTP sets clock | Drifts between syncs |
| v0.13.1 | dac957c | Hourly step of system clock to PCF85063 (edge-timed); second-aligned RTC writes | Patch; owner reports large daily loss on all boards |
| v0.14.0 | af3bb5a | `timesvc.c`: one module, lock, monotonic scheduling, wake check, recovery policy, online drift learning | Patch; superseded |
| v0.15.0 | 1f8728e | **New clock layer from scratch**: `clock.c` + `clock_rtc.c` (see section 5) | Compiles on all boards; not run on hardware; does not yet conform to the spec (section 6) |
| v0.15.1-4 | 2c363f5..8f746c5 | **Clock lab firmware** (`clocklab.c`) for measurement | Two runs completed (section 4) |

## 4. Measurements (clock lab)

### 4.1 Method [verified: `main/clocklab.c`]

Lab firmware (`CONFIG_WATCH_CLOCK_LAB=y`, no UI, same peripheral power-down as the watch) runs blocks of
a single activity. Each block is bracketed by an SNTP measurement taken through the overridden
`sntp_sync_time()` hook (UTC plus the esp_timer instant it arrived; the clock is never set), radio off
in between. Every `esp_light_sleep_start()` is timed with esp_timer immediately before and after;
S = summed successful sleep windows, N = count. Requested durations are not used. Error =
(esp_timer elapsed) - (network elapsed); negative = the watch loses time. On PCF85063 boards the RTC is
read on its seconds rollover at both ends (not yet run on such a board). Results persist in NVS and
print over USB.

SNTP endpoint noise: awake blocks scatter by roughly +-0.3 s over 30 min, i.e. a few hundred ppm on a
30-minute block. Treat single-block ppm values below ~300 ppm as noise.

### 4.2 Run 1: 1.75C, RC_FAST/256 (current watch configuration), 3 cycles, 13.4 h [measured]

| Block | Sleep length | Error per sleep | Rate (3 cycles) |
|---|---|---|---|
| AWAKE | none, 30 min | - | -42, -58, +258 ppm (noise) |
| FIFTH | 0.2 s x 3000 | -1.5 ms | -7524, -7429, -7480 ppm |
| SEC | 1 s x 1800 | -7.5 ms | -7570, -7491, -7508 ppm |
| MIN | 60 s x 60 | -449 ms | -7545, -7487, -7422 ppm |
| PROD | 900 s x 4 | -6.08 s | -6644, -6707, -6914 ppm |
| LONG | 1800 s x 2 | -9.29 s | -4864, -5504, -5114 ppm |

Battery 4.06 V -> 3.89 V over the run; no trend of error with voltage visible.

### 4.3 Run 2: 1.75C, RC_SLOW (136 kHz), 2 cycles, ~14 h [measured]

| Block | Sleep length | Cycle 0 | Cycle 1 |
|---|---|---|---|
| AWAKE | none | -34 ppm | +12 ppm |
| SEC | 1 s x 1800 | +6 ppm | +95 ppm |
| PROD | 900 s x 4 | +2061 ppm | +3310 ppm |
| LONG | 1800 s x 2 | +5827 ppm | +4140 ppm |
| HOUR | 3600 s x 1 | +7971 ppm | +9588 ppm |
| 3HOUR | 10800 s x 1 | +8208 ppm | +7267 ppm |

### 4.4 Findings

1. **The loss happens only in light sleep.** Awake timekeeping is within measurement noise. [measured]
2. **No per-transition error.** 0.2 s, 1 s and 60 s sleeps show the same rate on RC_FAST; the `b*N`
   term is effectively zero for this platform. [measured]
3. **The error depends on how long each sleep lasts, and restarts at every sleep entry.** RC_FAST starts
   about -7500 ppm and eases with length (-6750 at 15 min, -5160 at 30 min). RC_SLOW starts near
   0 ppm and rises to about +8000 ppm within an hour, with a lower 3 h average than 1 h. [measured]
   Both oscillators therefore speed up relative to their awake calibration as a sleep continues; the
   cause (regulator settling, thermal) is [inferred], not observed.
4. **Repeatability:** RC_FAST short sleeps +-1 %, 15 min +-2 %, 30 min +-6 % between cycles. RC_SLOW
   15 min +-23 %. Long sleeps are not reliably correctable on either. [measured]
5. **This explains the reported loss.** The 1.75-B wakes every 1 s (PWR polling) and the 1.75C every
   15 min (battery log): both run at -6700 to -7500 ppm, i.e. about 10 min/day. [measured rate,
   inferred link to the reports; the remaining gap to "15 min/day" is unexplained] [open]
6. **Design lever:** short sleeps. RC_SLOW at 1 s is nearly correct uncorrected (+6 / +95 ppm, only two
   samples); RC_FAST at 1 s is a stable -7500 ppm that one constant can correct. [measured, small n]
7. On RTC boards the sleep clock's error is irrelevant to displayed time **if** the RTC is re-read on
   every wake before drawing. The RTC's own accuracy is not yet measured. [inferred / open]

### 4.5 Run 3: 1.75C, RC_FAST/256, short sleeps, 3 cycles, ~12 h [measured]

| Sleep length | Cycle 0 | Cycle 1 | Cycle 2 (ppm of sleep time) |
|---|---|---|---|
| 0.25 s x 7200 | -7469 | -7504 | -7464 |
| 0.5 s x 3600 | -7546 | -7502 | -7501 |
| 1 s x 1800 | -7572 | -7515 | -7643 |
| 2 s x 900 | -7573 | -7500 | -7570 |
| 5 s x 360 | -7534 | -7550 | -7564 |
| 10 s x 180 | -7546 | -7518 | -7524 |
| 30 s x 60 | -7514 | -7446 | -7481 |
| Awake 15 min | +125 | +113 | +31 (ppm overall; endpoint noise ~+-100 ms) |

Findings:
- Every sleep from 0.25 s to 60 s (runs 1 and 3, two days, n = 30 blocks) runs at **-7516 ppm,
  sd 48 ppm**; run 1 mean -7495, run 3 mean -7526. The sd matches the SNTP endpoint noise
  (+-100 ms over a 30 min block = +-55 ppm), so the oscillator's own spread is at or below that. [measured]
- No dependence on sleep length within 0.25-60 s; the drift seen in runs 1-2 starts between 60 s
  and 15 min. [measured]
- Implied correction for sleep windows: multiply measured sleep time by 1/(1 - 7516e-6) = 1.0075733.
  [measured mean; to be validated]
- Design consequence [decision pending owner]: on the 1.75C, use RC_FAST/256, cap every light
  sleep at 30 s (one extra wake per 30 s, negligible battery), and apply the constant above via the
  holdover record. RC_SLOW is not needed for this path.

### 4.6 Running next

Lab format 4 (v0.15.5): AWAKE 15 min, then 6 x 2 h blocks of 30 s sleeps with a 5 s awake spell every
10 min (watch-like), PMIC die temperature logged at each block end. Purpose: confirm the constant
over 2 h blocks (endpoint noise ~+-15 ppm) and look for temperature dependence (desk run and a
warm/pocket run).

### 4.7 Earlier plan for run 3 (superseded)

Lab format 3 (v0.15.4): AWAKE 15 min, then 30 min each of 0.25, 0.5, 1, 2, 5, 10, 30 s sleeps, 3 cycles
(~12 h). Planned: 1.75C on RC_SLOW (variant B) and 1.75-B on RC_FAST (variant A, also measures RTC rate)
in parallel; swap the variants the next night to separate oscillator from chip-to-chip effects.
USB status now prints battery mV, % and charge state; the lab powers off when done (press PWR on USB to
read results, because USB cannot enumerate while the chip light-sleeps).

## 5. Current code (v0.15.0 clock layer)

- `main/clock_rtc.c` (PCF85063 driver, `pcf_*`): single-burst 7-byte calendar reads, full BCD/date
  validation, typed status (`PCF_OK/ABSENT/BUS/STOPPED/OSC_LOST/BAD_DATA/NO_TICK/VERIFY`), rollover
  detection with bracket width, write with STOP / load / release at S+0.492 s (datasheet 0.5078 s first
  increment) / restart on every path / read-back verification.
- `main/clock.c`: time = anchor (UTC at an esp_timer instant) + esp_timer elapsed; lock-free reads;
  states UNSET / ESTIMATED / READY; sources NONE / RETAINED / RTC / NETWORK; SNTP via overridden
  `sntp_sync_time()`; RTC read before each screen wake is drawn, refined on rollover afterwards and at
  least hourly; RTC written only from network time; sleep-error learning (online EMA) and correction;
  daily automatic Wi-Fi sync (weekly on RTC boards); faces show "Time not set" when UNSET.
- `main/clocklab.c`: measurement firmware (section 4).

## 6. Gap analysis: v0.15.0 against the owner's spec

Does not conform (must change):

- Online adaptive learning of the sleep error (spec 18.9 forbids in v1).
- Single additive ppm correction; no exact inverse gain, no fixed-point remainder carry (18.1, 18.4).
- Correction ignores sleep-length dependence (now known to be the dominant effect).
- `M_before` taken in prepare, not immediately before `esp_light_sleep_start()` (18.4).
- No uncertainty tracking or `uncertainty_known` (invariant 10, 18.5).
- No versioned calibration record / lifecycle (18.7).
- Silent daily Wi-Fi sync on by default (18.5, 18.8).
- Retained-over-reset time published as usable estimate without verification (18.4).
- Mutex held across light sleep instead of a prepare/resume handshake (section 10).
- Mutex-based ownership with SNTP applied in the network task, not a service task with a request
  queue (invariant 2, section 10).
- Hourly 1.3 s RTC edge poll on housekeeping wakes (sections 8, 11).
- STOP-release alignment used before simple verified writes were validated (section 9).
- No pure model module, no host tests, no standalone clock firmware gate (milestones 1-4).

Conforms: SNTP weak-hook override; typed, validated RTC driver; faces keyed on epoch second plus
generation; "Time not set" instead of a build-time dial; RTC written only from network time.

## 7. Plan

### Phase 1 - Characterise (in progress)
- Complete lab format 3 on 1.75C and 1.75-B, both oscillators, swapped on a second night.
- Add PMIC die temperature to the lab record as a coarse proxy; run once on a desk and once warm
  (pocket) to expose temperature dependence.
- Output: choice of slow-clock source and sleep length for the 1.75C; correction constant with spread;
  PCF85063 rate on the 1.75-B.

### Phase 2 - Clock core off the watch
- `clock_model.c/.h`: pure C, no OS/I2C. Signed 64-bit microseconds; UTC intervals (low/high); anchor;
  session id; generation; `uncertainty_known`; fixed-point gains with remainder carry.
- `internal_holdover.c/.h` (1.75C): model fitted from lab data. Given finding 3, the model is
  **per-sleep-duration**, not a single rate: the watch will cap each sleep at the chosen short length
  (e.g. 1 s) and apply the qualified correction for that length. Exact inverse gain. `b` retained in the
  model but expected ~0.
- Calibration record: versioned, checksummed, config fingerprint (oscillator, sleep length, SDK,
  firmware), states UNCALIBRATED / COLLECTING / CANDIDATE / QUALIFIED / INVALIDATED; loaded over USB.
- Offline fit tool (Python): weighted least squares from lab CSV; refuses ill-conditioned data.
- Host tests in CI covering spec 15 and 18.10 cases using the real model code.

### Phase 2b - Calibration policy [decision, 2026-11-24]
- No always-running adaptive service yet. During normal operation the watch only **collects**
  low-cost observations (A, S, N, sleep regime, PMIC temperature, reference endpoints) on wakes and
  references that already happen: no extra wakeups, no flash writes per sleep.
- Candidate models are **fitted offline** from those observations and lab data.
- **Automatic promotion** on the watch is added only if held-out tests show it improves accuracy;
  until then the current qualified model stays active.
- Auto-calibration, if built, is **opt-in**.
- 1.75C: an optional, visible daily Wi-Fi reference is a candidate tradeoff. Its energy is not
  measured (0.14 mAh/sync assumed 100 mA for 5 s); measure association time, failures and current
  first, and prefer reusing an already-scheduled sync.
- RTC boards: RTC reads on wakes that already happen are useful observations of the sleep clock,
  but repeated reads of the same RTC cannot establish the RTC's absolute accuracy. Offset-register
  calibration needs independent references over long intervals. Sleep-clock calibration is not
  needed on these boards while the RTC restores time directly.
- Model scope: duration effects justify separate regimes (short sleeps <= 60 s; long sleeps), not
  a per-length model; no temperature term unless measurements require it.

### Phase 3 - Service and integration
- One clock service task with a bounded request queue; lock-free snapshot reads.
- Sleep prepare/resume handshake with token; no lock across sleep; `M_before`/`M_after` taken adjacent to
  `esp_light_sleep_start()`.
- RTC boards: coarse RTC read before the first time-bearing frame on every display wake; optional phase
  refine only on display wakes; no polling on housekeeping wakes.
- SNTP measurement delivered to the service (hook already done); results distinguish received /
  accepted / persisted.
- 1.75C: holdover from qualified record; DEGRADED with `uncertainty_known = false` until qualified;
  automatic sync only as an explicit, visible setting.
- Controller: 1.75C screen-off sleep split into the qualified short sleep length.
- Remove online learning, the mutex-across-sleep, and old paths in the same change.

### Phase 4 - Acceptance per board
- 24-72 h offline, no network correction during the interval, first displayed time after wake checked
  against a reference; resets, PMIC power-off, button wakes, rollovers.
- Target <= 2 s/day on RTC boards; 1.75C target set from measured residual and battery cost, reported to
  the owner for decision.

### Phase 5 - Resume battery work
- Time acceptance tests become required regression gates.

## 8. Open questions and decisions needed

1. Which firmware versions and intervals produced the "15 min/day on all boards" observation? [open]
2. The <= 2 s/day target for the 1.75C: confirm, or set from Phase 1 results. 2 s/day is about 23 ppm;
   short-sleep spread measured so far is +-70 ppm (RC_FAST) and unknown for RC_SLOW (2 samples). [open]
3. Battery cost of 1 s sleep slicing on the 1.75C: estimated around 1 % (a few ms awake per second);
   to be measured. [inferred]
4. Temperature sensitivity: not yet measured. [open]
5. Whether an opt-in daily Wi-Fi sync is acceptable for the 1.75C as a visible setting. [open]

## 9. Raw data

Format: `format,variant,cycle,block,name,n_ok,n_rej,req_ms,S_us,ref0_utc,ref0_mono,ref1_utc,ref1_mono,rtc0_s,rtc0_mono,rtc1_s,rtc1_mono,mv0,mv1`
(variant 1 = RC_FAST/256, 2 = RC_SLOW; times in microseconds; error = (ref1_mono-ref0_mono) - (ref1_utc-ref0_utc)).

Run 1 (1.75C, RC_FAST/256):
```
1,1,0,0,AWAKE,0,0,0,0,1795297905850034,15935979,1795299720990017,1831000139,0,0,0,0,4055,4015
1,1,0,1,LONG,2,0,1800000,3599991673,1795299727208045,1837218883,1795303353954739,5446453466,0,0,0,0,4009,4023
1,1,0,2,MIN,60,0,60000,3600006302,1795303360983808,5453479282,1795306994848404,9060183047,0,0,0,0,4008,4018
1,1,0,3,SEC,1800,0,1000,1800172867,1795307000865606,9066192583,1795308827567217,10879266377,0,0,0,0,4004,4016
1,1,0,4,FIFTH,3000,0,200,600296469,1795308839428605,10891128668,1795309455995948,11503179422,0,0,0,0,4001,4014
1,1,0,5,PROD,4,0,900000,3600000445,1795309544905512,11592089375,1795313260713748,15283978246,0,0,0,0,3995,4006
1,1,1,0,AWAKE,0,0,0,0,1795313274257535,15297613210,1795315136362682,17159610048,0,0,0,0,3970,3964
1,1,1,1,LONG,2,0,1800000,3600000219,1795315160054068,17183374365,1795318794756358,20798262445,0,0,0,0,3947,3973
1,1,1,2,MIN,60,0,60000,3600006014,1795318801991000,20805520852,1795322439987716,24416563113,0,0,0,0,3939,3968
1,1,1,3,SEC,1800,0,1000,1800171479,1795322449834535,24426393605,1795324274480483,26237554330,0,0,0,0,3953,3965
1,1,1,4,FIFTH,3000,0,200,600290695,1795324282221218,26245237035,1795324898551146,26857107511,0,0,0,0,3950,3964
1,1,1,5,PROD,4,0,900000,3600000423,1795324909210291,26867859804,1795328548405752,30482910272,0,0,0,0,3948,3958
1,1,2,0,AWAKE,0,0,0,0,1795331075493999,33007439353,1795332884299718,34816711321,0,0,0,0,3908,3899
1,1,2,1,LONG,2,0,1800000,3600000159,1795332894125110,34826747041,1795336536185192,38450396080,0,0,0,0,3893,3915
1,1,2,2,MIN,60,0,60000,3600006198,1795336559419574,38473639371,1795340194767485,42082268871,0,0,0,0,3897,3912
1,1,2,3,SEC,1800,0,1000,1800170361,1795340203585511,42090683546,1795342022794607,43896376264,0,0,0,0,3878,3909
1,1,2,4,FIFTH,3000,0,200,600291279,1795342031651627,43905233260,1795342645500285,44514591929,0,0,0,0,3894,3907
1,1,2,5,PROD,4,0,900000,3600000439,1795342654719406,44523813843,1795346284510097,48128715446,0,0,0,0,3893,3903
```

Run 2 (1.75C, RC_SLOW):
```
2,2,0,0,AWAKE,0,0,0,0,1795374053585887,2428873386,1795375864172283,4239398586,0,0,0,0,4143,4066
2,2,0,1,SEC,1800,0,1000,1800175382,1795375872513161,4247858552,1795377680445500,6055800895,0,0,0,0,4061,4072
2,2,0,2,PROD,4,0,900000,3600000451,1795377686275511,6061694315,1795381284798365,9667634992,0,0,0,0,4059,4066
2,2,0,3,LONG,2,0,1800000,3600000234,1795381290753895,9673650064,1795384875104851,13278887033,0,0,0,0,4054,4061
2,2,0,4,HOUR,1,0,3600000,3600000183,1795384881042779,13284826596,1795388459873189,16892183422,0,0,0,0,4049,4058
2,2,0,5,3HOUR,1,0,10800000,10800000123,1795388468849231,16901092908,1795399188607362,27708842817,0,0,0,0,4046,4049
2,2,1,0,AWAKE,0,0,0,0,1795399194780990,27714967608,1795401025618472,29545827496,0,0,0,0,4032,4022
2,2,1,1,SEC,1800,0,1000,1800183933,1795401034865406,29555065114,1795402866470179,31386840821,0,0,0,0,4019,4036
2,2,1,2,PROD,4,0,900000,3600000454,1795402872262626,31392577028,1795406468759557,35000977408,0,0,0,0,4021,4033
2,2,1,3,LONG,2,0,1800000,3600000233,1795406476409430,35008657494,1795410068861013,38615983007,0,0,0,0,4004,4030
2,2,1,4,HOUR,1,0,3600000,3600000179,1795410074431632,38621614739,1795413649437929,42230899328,0,0,0,0,4018,4026
2,2,1,5,3HOUR,1,0,10800000,10800000127,1795413658134034,42239650004,1795424388254103,53047749230,0,0,0,0,4015,4013
```
