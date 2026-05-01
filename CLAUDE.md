# CLAUDE.md — parallel-qemu/

This is a **submodule** of the QFlex simulator. The parent repo is at `..`; its [CLAUDE.md](../CLAUDE.md) explains the four-phase pipeline, sampling vocabulary, and how the binaries are wired into per-experiment `run/` directories. Read it for the full picture; this file documents only what's specific to *parallel-qemu/*.

## What this submodule is

A **PARSA-EPFL fork of upstream QEMU** (aarch64-softmmu target). Built **without** `--enable-libqflex` — this is the "fast" QEMU, distinct from the timing fork at [../qemu/](../qemu/).

> **Important: this file documents only the QFlex-specific delta from upstream QEMU.** Upstream QEMU is a huge project with its own docs at `docs/` and `README.rst`; do not re-document it here. If you need to know upstream behaviour, read those — but for QFlex-specific concerns, this file is the entry point.

## Role in QFlex

Used in the first three phases:

- **Phase 1 (emulation):** boot Alpine guest, install/configure workload, `savevm base`.
- **Phase 2 (functional warming):** replay workload with [WormCacheQFlex](../WormCacheQFlex/) attached as a `-plugin`. WormCache models long-term µarch state.
- **Phase 3 (sample selection on FW):** emit one checkpoint per sampling unit along the FW timeline.

After build, the parent stages this binary at `<parent>/parallel-qemu-saved/build/qemu-system-aarch64` and `set_up_folders()` symlinks it into per-experiment `run/` as the **unprefixed** `qemu-system-aarch64` (see [../commands/config.py:286-303](../commands/config.py#L286)). The `vanilla-` prefixed binary in `run/` is the *other* fork ([../qemu/](../qemu/)).

## What's QFlex-specific (the only thing this file documents)

### `net/pdes-*.c` — multi-node PDES networking

The entire **PDES (Parallel Discrete Event Simulation)** stack is QFlex-only:

| File | Role |
|---|---|
| `net/pdes-netdev.c` | Registers the `NET_CLIENT_DRIVER_PDES` network backend. Also added to `qapi/net.json`. |
| `net/pdes-engine.c` | Singleton engine — virtual-time tracking, pause/play, quantum scheduling across neighbours. |
| `net/pdes-wwt.c` | Wisconsin Wind Tunnel (WWT) synchronisation protocol for cross-node message ordering. |
| `net/pdes-communicator.c` | POSIX shared-memory ring buffers via `shm_open()`. Send/recv rings named `/dev/shm/pdes_<from>_to_<to>`. |
| `net/pdes-checkpoint.c` | Drains in-flight messages before savevm so distributed snapshots stay consistent. |
| `net/pdes-utility.c` | Helpers. |

The parent's `ExperimentContext.pdes_net_devs` field selects between `e1000` and `virtio-net-pci` as the **guest-facing** NIC; the host side always speaks PDES via shm. `neighbor_node_list`, `latencies_ns_list`, `syncs_list` (parallel arrays on `ExperimentContext`) configure the topology.

The same `net/pdes-*.c` files exist in [../qemu/](../qemu/) — the two forks are kept in sync on this stack.

### Snapshot integration

Standard `savevm`/`loadvm` HMP commands work as-is, but `pdes_drain()` in `net/pdes-checkpoint.c` is invoked to drain pending PDES messages first, so distributed snapshots are coherent.

### What's NOT QFlex-specific

- **No `--enable-libqflex` or `--enable-pdes` configure flag.** PDES is unconditionally compiled in (it's just normal QEMU source files). This fork's QFlex configure line is `./configure --target-list=aarch64-softmmu --disable-gtk --enable-capstone` — purely upstream knobs.
- **Plugin loading is the standard QEMU `-plugin` API.** No QFlex-private plugin extension here. WormCacheQFlex is loaded via the unmodified upstream plugin mechanism.

## Build

From the parent repo:

```sh
make parallel-qemu-config
make parallel-qemu-build
```

Output: `build/qemu-system-aarch64`. The parent then `cp`s `build/` to `<parent>/parallel-qemu-saved/build/`.

## Public interface to the rest of QFlex

- **WormCacheQFlex loading:** `-plugin lib/libworm_cache.so,mode=...,...` — see [../WormCacheQFlex/CLAUDE.md](../WormCacheQFlex/CLAUDE.md) for the arg format.
- **Multi-node shm:** `/dev/shm/pdes_<from>_to_<to>` ring buffers. The **master node (node 0)** is responsible for clearing stale shm files before launch; non-master nodes don't. After a crashed run, use [../clean_up.sh](../clean_up.sh) to remove `/dev/shm/pdes*` and kill lingering qemu processes before retrying.
- **Snapshots:** unmodified `savevm` / `loadvm` HMP commands; PDES-aware via `pdes_drain()`.

## Conventions and gotchas

- **All recent commits are PARSA-specific.** Don't `git pull --rebase` from upstream-qemu without expecting conflicts in `net/`. Check `git log --oneline -20` to see the QFlex history.
- **Multi-node needs real shared memory.** The parent's docker invocation uses `--shm-size=128g` and `--pid=host` for a reason — small shm = silent multi-node breakage.
- **No QFlex-specific docs in this repo** — `README.rst` and `docs/` are upstream QEMU. The PDES headers (`net/pdes-*.h`) have inline comments on message format and virtual time, which is the closest thing to architecture docs.
- **Two forks, kept in sync on PDES.** If you change `net/pdes-*.c` here, the same change is likely needed in [../qemu/](../qemu/).
- The submodule lives on branch `temp/multi-node-tmp-solution`. Recent work: PDES checkpoint races, pause/play sync, timer alignment.

## See also

- [../CLAUDE.md](../CLAUDE.md) — qflex root: four-phase pipeline, multi-node config (`neighbor_node_list`, `latencies_ns_list`, `syncs_list`, `pdes_net_devs`), `ExperimentContext`.
- [../WormCacheQFlex/CLAUDE.md](../WormCacheQFlex/CLAUDE.md) — the Rust plugin this QEMU loads via `-plugin` during functional warming.
- [../qemu/CLAUDE.md](../qemu/CLAUDE.md) — the *other* QEMU fork (timing). PDES networking is kept in sync between the two; libqflex is built into that one only.
- [../qemu/middleware/CLAUDE.md](../qemu/middleware/CLAUDE.md) — the QEMU↔Flexus shim used by the timing fork (not embedded here).
- [../flexus/CLAUDE.md](../flexus/CLAUDE.md) — the timing model. Not loaded here; this fork only does emulation, FW, and sample selection.
