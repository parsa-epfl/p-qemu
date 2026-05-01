# MULTI_NODE.md — parallel-qemu/

This file documents what *parallel-qemu* contributes to QFlex multi-node and how it relies on the other submodules. The cross-cutting overview (PDES wire, WWT, virtual-time sourcing, sampling-unit alignment, distributed snapshots) is in [../MULTI_NODE.md](../MULTI_NODE.md). Read that first if you haven't.

For this submodule's general context (role, build, layout) see [CLAUDE.md](CLAUDE.md).

## What this submodule contributes to PDES

- **Owns the canonical PDES code.** All six `net/pdes-*.c` files originated here and live here as the source of truth:
  - `net/pdes-netdev.c` — `NET_CLIENT_DRIVER_PDES` registration.
  - `net/pdes-engine.c` — singleton `PDESEngine`.
  - `net/pdes-wwt.c` — Wisconsin Wind Tunnel quantum sync.
  - `net/pdes-communicator.c` — POSIX-shm rings.
  - `net/pdes-checkpoint.c` — distributed snapshot drain / in-flight message persistence.
  - `net/pdes-utility.c` — helpers.
  Headers in `include/net/pdes-*.h`. The [qemu/](../qemu/) fork carries an in-sync copy.

- **Provides virtual time during phases 1–3 (emulation, FW, sample selection).** The clock comes from QEMU's icount machinery, surfaced via `-icount shift=…,q=<quantum_size>,…` (built by [../commands/qemu.py:95-103](../commands/qemu.py#L95) `quantum_args()`). PDES queries this clock to decide WWT quantum boundaries and to timestamp outgoing messages.

- **Hosts the singleton `PDESEngine` and `PDESWWT`** during FW. Same singleton-per-process model as the timing fork.

- **Gets distributed-snapshot drain right for FW checkpoints.** When [WormCacheQFlex](../WormCacheQFlex/) fires `savevm_cb`, the PDES engine drains in-flight messages (`DRAIN_START` / `DRAIN_END` exchange) and persists them to JSON before the FW checkpoint lands. This is what guarantees the per-sampling-unit checkpoints are coherent across nodes.

## How this submodule uses the other submodules

- **[../WormCacheQFlex/](../WormCacheQFlex/)** — loaded as `-plugin lib/libworm_cache.so,…` during FW. Multi-node-relevant: WormCache's `savevm_cb` is the trigger for the distributed-snapshot drain machinery in `net/pdes-checkpoint.c`. WormCache itself doesn't know about PDES; it just signals "snapshot now" and the engine handles cross-node coherence.

- **[../qemu/](../qemu/)** — mirrors `net/pdes-*.c` between the two forks. **Changes here usually need to be replicated there**, otherwise the FW and timing phases drift apart on the wire format or sync protocol.

- **[../qemu/middleware/](../qemu/middleware/)** and **[../flexus/](../flexus/)** — not used during phases 1–3. They only matter once the run hands off to the timing phase (different binary, different clock source).

## Phase-specific PDES invocation

Multi-node parallel-qemu launches via [../commands/multinode.py](../commands/multinode.py) (`./qflex multi`), which today is a thin wrapper:

```
gdb --args ./qemu-system-aarch64 <base args> <quantum_args>
```

Each node has to be started separately; the master must come up first because it's the one that clears stale `/dev/shm/pdes*` rings ([../commands/config.py:362-369](../commands/config.py#L362)).

The PDES backend is wired in by `setup_nic_args()` ([../commands/config.py:351-396](../commands/config.py#L351)) which builds, per neighbour, a QEMU CLI fragment like:

```
-netdev pdes,id=net<i>,shm-send=/<send>,shm-recv=/<recv>,latencyns=<L>,sync=<true|false>,master=<true|false>
-device <e1000|virtio-net-pci>,netdev=net<i>,mac=52:54:00:aa:bb:<node*10+i>
```

That `pdes` netdev type is the QFlex-only `NET_CLIENT_DRIVER_PDES` registered in `net/pdes-netdev.c`.

## See also

- [../MULTI_NODE.md](../MULTI_NODE.md) — the comprehensive cross-cutting overview.
- [../qemu/MULTI_NODE.md](../qemu/MULTI_NODE.md) — the *other* QEMU fork (timing). PDES code is mirrored between the two; clock source differs.
- [../qemu/middleware/MULTI_NODE.md](../qemu/middleware/MULTI_NODE.md) — pause handshake and Flexus tick → virtual time (timing phase only; not used here).
- [../flexus/MULTI_NODE.md](../flexus/MULTI_NODE.md) — cycle-count clock for the timing phase.
- [../WormCacheQFlex/MULTI_NODE.md](../WormCacheQFlex/MULTI_NODE.md) — fires `savevm_cb` which triggers the distributed-snapshot drain that this fork implements.
