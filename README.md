# QNX Periodic Telemetry Codelab

A step-by-step tutorial for building and running a small periodic sampler on
QNX Neutrino 8.0. It shows two things:

1. **Drift-free periodic timing** with `clock_nanosleep()` and an absolute
   `CLOCK_MONOTONIC` deadline.
2. **QNX native message passing** (`ChannelCreate`, `ConnectAttach`,
   `MsgSend`, `MsgReceive`, `MsgReply`) between a `SCHED_FIFO` sampler thread
   and a logger thread.

The pattern is the same one used for periodic sensor reads in robotics and
medical-device style workloads: wake on a fixed schedule, take a reading, hand
it off, go back to sleep. This repository is a learning example. It is not a
certified, safety-rated or production component.

Entry for the QNX Global Hackathon 2026.

| Submission detail | Value |
|---|---|
| Category | Robotics & Medical |
| QNX version | QNX 8.0.0 (QNX SDP 8.0) |
| Target | x86_64 virtual machine in VirtualBox, image built with `mkqnximage --type=vbox` |
| Toolchain | `qcc -Vgcc_ntox86_64` from QNX SDP 8.0, on a Windows host |
| Where QNX is used | `src/qnx_monitor.c`: `ChannelCreate()`, `ConnectAttach()`, `MsgSend()`, `MsgReceive()`, `MsgReply()`, `MsgError()`, `ClockCycles()`, `SCHED_FIFO` threads; checked on the target with `pidin` (see the API table below) |

---

## Repository structure

```
qnx-telemetry-codelab/
├── README.md            # This tutorial
├── Makefile             # Builds with qcc -Vgcc_ntox86_64 by default
├── LICENSE              # MIT
└── src/
    └── qnx_monitor.c    # Sampler + logger, one process
```

## What it is

`qnx_monitor` is one process with three threads:

| Thread  | Scheduling                    | Job |
|---------|-------------------------------|-----|
| main    | default                       | Sets up the channel and threads, catches Ctrl+C / `kill`, waits for the others |
| sampler | `SCHED_FIFO`, priority 50     | Wakes every 100 ms on an absolute deadline, builds a sample, sends it with `MsgSend()` |
| logger  | default (runs at 50 while receiving, see Step 6) | Receives samples with `MsgReceive()`, replies with `MsgReply()`, prints each sample and the final summary |

All printing happens in the logger, so the sampler does no console I/O inside
its loop.

### Why absolute deadlines

The first version of this codelab slept with `delay(100)` after each sample.
A relative sleep starts counting *after* the work and any scheduling delay,
so every cycle adds a little extra time and the intervals drift above 100 ms.

This version keeps a `next` deadline and advances it by exactly 100 ms every
cycle:

```c
clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
/* take sample ... */
next += 100 ms;   /* tv_nsec overflow carried into tv_sec */
```

Lateness in one cycle no longer pushes every later cycle back. `EINTR` is
retried.

### What gets measured

For every sample the logger prints:

- **interval**: time since the previous sample (the first sample is measured
  from program start)
- **jitter**: interval minus the 100 ms target
- **wake_late**: how far after its deadline the sampler actually woke
- **skipped**: periods skipped just before this sample (see below)
- **cycles**: `ClockCycles()` count since the sampler started

At exit it prints min, max and average interval, max absolute jitter, and the
missed-deadline count.

### Missed-deadline policy

After each sample is sent, the deadline moves forward one period. If the
current time is already past that new deadline, the deadline counts as
missed. The sampler then skips ahead to the next period that is still in the
future and counts every skipped period as missed. It never fires a burst of
back-to-back samples to "catch up". Samples stay on the original 100 ms grid,
so the interval after a skip is longer than 100 ms, and it can be shorter if
the previous sample itself woke late.

### Message flow

```
sampler (FIFO 50)                         logger (default prio)
-----------------                         ---------------------
ConnectAttach(chid) ──┐                   ChannelCreate()  (done in main)
                      │                   MsgReceive(chid) ... blocked
MsgSend(MSG_SAMPLE) ──┼──────────────────▶ receives sample
   (blocked)          │◀────────────────── MsgReply(EOK), then prints
  ...                 │
MsgSend(MSG_SHUTDOWN)─┼──────────────────▶ prints summary
   (blocked)          │◀────────────────── MsgReply(EOK), thread exits
ConnectDetach()
```

Message types start above `_IO_MAX` so they don't overlap the range used by
resource-manager I/O messages. Unknown messages get `MsgError(ENOSYS)` so a
sender is never left blocked. Every kernel call's return value is checked.

## What it isn't

- Not a real-time guarantee. It measures timing; it doesn't promise it.
- Not safety-certified, and not a medical or robotics product.
- Not a benchmark. Numbers from a VM say little about real hardware.
- Not a multi-process design. Both ends of the channel live in one process
  to keep the example short.

---

## Setup used for this codelab

- Windows host
- QNX SDP 8.0 (installed with QNX Software Center)
- VirtualBox, with a QNX 8.0.0 x86_64 VM built by `mkqnximage --type=vbox`
  on a VirtualBox host-only network
- Windows OpenSSH (`ssh` / `scp`) to copy and run the binary as `qnxuser`

Paths below assume the SDP is in `%USERPROFILE%\qnx800`. Change them if yours
is elsewhere.

## 1. Set up the QNX environment (Windows)

Open **Command Prompt** (cmd.exe) and run:

```bat
%USERPROFILE%\qnx800\qnxsdp-env.bat
qcc -V
```

`qcc -V` lists the available targets. You should see `gcc_ntox86_64`.
Run `qnxsdp-env.bat` again in every new Command Prompt window.

## 2. Get the code and build

```bat
git clone https://github.com/CreigT/qnx-telemetry-codelab-.git
cd qnx-telemetry-codelab-
```

Build with make:

```bat
make
```

Or call qcc directly:

```bat
qcc -Vgcc_ntox86_64 -std=gnu11 -Wall -Wextra -O2 -g -o qnx_monitor src\qnx_monitor.c
```

Both produce `qnx_monitor`, an x86_64 QNX binary. It will not run on Windows.

Useful make options:

```bat
make clean
make QCC_VARIANT=gcc_ntoaarch64le   :: 64-bit ARM target instead of x86_64
make CC=C:\path\to\qcc              :: use a specific qcc
```

## 3. Create and start the QNX VM

In VirtualBox, make sure a **host-only network** exists
(File > Tools > Network Manager). Then, in a Command Prompt where you've run
`qnxsdp-env.bat`:

```bat
mkdir %USERPROFILE%\qnx-vm
cd %USERPROFILE%\qnx-vm
mkqnximage --type=vbox --run
```

`mkqnximage --help` lists the other options. When the VM has booted, log in
on its console and find its address:

```sh
ifconfig
```

Use the host-only address wherever this guide says `<vm-ip>`.

## 4. Copy the binary to the VM

Windows OpenSSH and the QNX image don't agree on default ciphers, so pass the
cipher and MAC explicitly. From the repository folder on Windows:

```bat
scp -c aes128-ctr -o MACs=hmac-sha2-256 qnx_monitor qnxuser@<vm-ip>:
```

The trailing `:` copies into `qnxuser`'s home directory.

## 5. Run it

```bat
ssh -c aes128-ctr -o MACs=hmac-sha2-256 qnxuser@<vm-ip>
```

Then, on the VM:

```sh
chmod +x ~/qnx_monitor
~/qnx_monitor          # 100 samples, about 10 seconds
~/qnx_monitor 600      # 600 samples, about 1 minute
~/qnx_monitor 0        # run until you stop it
```

If the sampler thread can't get `SCHED_FIFO` priority 50, the program prints
`Could not start SCHED_FIFO sampler ...` and exits instead of running at the
wrong priority.

### Output format

This shows the **format only**. The `…` values are placeholders, not measured
results. Your numbers depend on the host, the VM and its load.

```
QNX periodic telemetry sampler
Target period: 100.000 ms | Samples: 100
Stop early with Ctrl+C or kill -s INT <pid>; the summary still prints.

[MAIN] Sampler thread running: priority=50 policy=FIFO
[MAIN] Check it with: pidin -p qnx_monitor

[LOG] #   0 interval=  …… ms jitter=  ±… ms wake_late=  … ms skipped=0 cycles=…
[LOG] #   1 interval=  …… ms jitter=  ±… ms wake_late=  … ms skipped=0 cycles=…
...

=== Summary (target period 100.000 ms) ===
Samples taken     : 100
Interval min      : … ms
Interval max      : … ms
Interval avg      : … ms
Max |jitter|      : … ms
Missed deadlines  : …
[MAIN] Done.
```

When stopped by a signal, the "Samples taken" line ends with
`(stopped by signal)`.

## 6. Check the thread priority

While it's running (use `~/qnx_monitor 0` so it keeps going), open a second
Command Prompt and connect again:

```bat
ssh -c aes128-ctr -o MACs=hmac-sha2-256 qnxuser@<vm-ip>
```

```sh
pidin -p qnx_monitor
```

The sampler thread should show priority **50f** (`f` = FIFO). The logger
thread is created at normal priority, but it usually shows **50r** (`RECEIVE`
state): QNX message passing gives a receiving thread the priority of the
thread that sent the message, so the logger runs at the sampler's priority
50 while it handles samples. The main thread shows the process's default
priority.

## 7. Stop it

Either of these stops it cleanly, and the summary still prints:

- **Ctrl+C** in the session running it (SIGINT)
- `kill -s INT <pid>` from another session. Get the pid from
  `pidin -p qnx_monitor` (the first column).

`slay` matches by process name and may not find the program depending on how
it was launched (for example, `slay` reported "Unable to find process" for a
binary started as `/tmp/qnx_monitor2`), so prefer `kill -s INT <pid>`.

The sampler checks for a stop request once per period, so it can take up to
about 100 ms to exit.

---

## Limits

- **VM timing isn't hardware timing.** VirtualBox shares the host CPU and
  timers with Windows. Intervals and jitter from a VM aren't representative of
  QNX on dedicated hardware.
- **`MsgSend()` is synchronous.** The sampler stays blocked until the logger
  replies, so every sample adds a short, variable delay to the sampler
  thread. The logger replies before printing to keep that short, but if
  printing takes longer than one period, the next send waits. A
  non-blocking alternative is a **pulse** (`MsgSendPulse()`), which carries
  only a small code and value and doesn't wait for a reply. A real design
  might pair pulses with a shared-memory ring buffer for the full sample data.
- **Single-process demo.** The channel is used inside one process. In a real
  system the logger would usually be its own process, found by name (for
  example with `name_attach()` / `name_open()`).
- **Not safety-certified.** Nothing here has been assessed against any safety
  or medical standard.
- **Stop latency.** Stopping can take up to one period.
- **Simulated reading.** The "sample" is a timestamp and a cycle count; there
  is no sensor driver.

## QNX concepts in this codelab

| Concept | Where |
|---------|-------|
| `PTHREAD_EXPLICIT_SCHED`, `SCHED_FIFO`, priority 50 | `main()` |
| `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` | `sampler_thread()` |
| `ChannelCreate()` / `ChannelDestroy()` | `main()` |
| `ConnectAttach(..., _NTO_SIDE_CHANNEL, ...)` / `ConnectDetach()` | `sampler_thread()` |
| `MsgSend()` | `sampler_thread()`, `send_shutdown()` |
| `MsgReceive()` / `MsgReply()` / `MsgError()` | `logger_thread()` |
| `ClockCycles()` | `sampler_thread()` |
| `pidin` to inspect thread priority | Step 6 |
| `qcc` cross-compilation | `Makefile` |

## Demo video checklist (2 minutes max)

1. Run `qnxsdp-env.bat` and `make` on Windows.
2. Copy the binary with `scp` and start it over `ssh`.
3. Show `pidin -p qnx_monitor` with the sampler at `50f`.
4. Stop it with Ctrl+C and show the summary.

## Ideas for extensions

- Replace the per-sample `MsgSend()` with `MsgSendPulse()` plus a shared ring buffer
- Move the logger into its own process with `name_attach()`
- Read a real sensor in place of the timestamp
- Add a histogram of intervals to the summary

## License

MIT. See [LICENSE](LICENSE).

---

Sponsored by CREIGNIFICENT LLC
