# QNX Real-Time Telemetry Codelab

**A hands-on tutorial for the QNX Global Hackathon 2026 — Best Codelab / Tutorial track.**

Build and run a minimal real-time C telemetry daemon on QNX Neutrino 8.0.  
Learn environment setup, cross-compilation with `qcc`, priority scheduling, and how to verify real-time behavior on QEMU or hardware.

---

## What You Will Build

A small daemon (`qnx_monitor`) that:

- Creates a high-priority `SCHED_FIFO` thread (priority 50)
- Samples system timing every 100 ms using `ClockCycles()`
- Prints clear telemetry so you can observe deterministic behavior
- Lets you confirm the priority with the classic `pidin` utility

This is the foundation for any mission-critical telemetry, sensor fusion, or control loop on QNX.

## Prerequisites

1. **QNX Everywhere** free non-commercial license  
   → [Get it here](https://www.qnx.com/developers/docs/qnxeverywhere/) (or follow the official onboarding video).
2. **QNX SDP 8.0** installed (typically at `~/qnx800`).
3. A Linux host (recommended) with QEMU for the easiest demo, or a Raspberry Pi running the QNX quickstart image.

## 1. Set Up the Environment

```bash
# Source the SDP environment (adjust path if needed)
source ~/qnx800/qnxsdp-env.sh

# Verify the compiler is available
qcc -V
```

You should see variants such as `gcc_ntox86_64` and `gcc_ntoaarch64le`.

## 2. Clone and Build

```bash
git clone https://github.com/CreigT/qnx-telemetry-codelab-.git
cd qnx-telemetry-codelab-

# Default target is x86_64 (perfect for QEMU)
make

# Or explicitly for ARM (Raspberry Pi)
# make QCC_VARIANT=gcc_ntoaarch64le
```

The binary `qnx_monitor` is now ready.

## 3. Run on QEMU (Recommended for Judges)

### Quick QEMU image

```bash
mkdir -p ~/qnx-demo && cd ~/qnx-demo
mkqnximage --type=qemu --arch=x86_64 --noprompt --build
mkqnximage --run
```

Once the serial console appears, copy the binary over (from another terminal):

```bash
# From your host, while the VM is running
scp qnx_monitor root@<qemu-ip>:/tmp/
# or use the shared folder / fat mount method shown in the QNX docs
```

On the QNX console:

```bash
chmod +x /tmp/qnx_monitor
/tmp/qnx_monitor
```

### Expected Output

```
QNX Real-Time Telemetry Monitor
Target: Neutrino 8.0 | Sample rate: 100 ms

[MAIN] Created RT thread. Waiting for completion...
[RT] Telemetry thread started. Priority=50 Policy=FIFO
[RT] Sample  0 | prio=50 | cycles=... | mono=...
[RT] Sample  1 | prio=50 | cycles=... | mono=...
...
[RT] Telemetry thread finished (30 samples).
[MAIN] Done. Use 'pidin -p qnx_monitor' while running to verify priority.
```

## 4. Verify Real-Time Priority

While the program is running (or immediately after), on the QNX target:

```bash
pidin -p qnx_monitor
```

You should see the worker thread at priority **50** with policy **FIFO** (shown as `50f` or similar).

## Project Layout

```
qnx-telemetry-codelab-/
├── README.md          ← you are here (the tutorial)
├── Makefile            ← cross-compilation with qcc
└── src/
    └── qnx_monitor.c    ← real-time telemetry daemon
```

## Key QNX Concepts Demonstrated

| Concept | Where |
|---------|-------|
| Explicit scheduling (`PTHREAD_EXPLICIT_SCHED`) | `qnx_monitor.c` |
| `SCHED_FIFO` + priority 50 | `qnx_monitor.c` |
| `ClockCycles()` for high-resolution timing | `qnx_monitor.c` |
| `delay()` for periodic sampling | `qnx_monitor.c` |
| `pidin` to inspect thread priority | README step 4 |
| `qcc -Vgcc_ntox86_64` cross-compilation | Makefile |

## Demo Video Checklist (2 minutes)

Judges love seeing it work. Record:

1. `source ~/qnx800/qnxsdp-env.sh`
2. `make` → successful build with `qcc`
3. Boot QEMU (`mkqnximage --run`)
4. Run `./qnx_monitor` and show the priority output
5. Quickly run `pidin -p qnx_monitor` to confirm priority 50

## Next Steps / Extensions

- Add `/proc` sampling for CPU and memory
- Use a QNX pulse or channel for IPC between threads
- Integrate with the Sensor Framework or publish over MQTT
- Port the same pattern to a Raspberry Pi 5 image

## License

MIT — free for the community and the hackathon.

---

**Built for the QNX Global Hackathon 2026**  
Best Codelab / Tutorial category.

Happy hacking — and keep it real-time.
