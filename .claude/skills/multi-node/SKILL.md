---
name: multi-node
description: QFlex multi-node from the parallel-qemu (fast QEMU) perspective — owner of the canonical PDES code (net/pdes-*.c), virtual-time source via icount during phases 1–3, target of the `multi` Typer command, host of the singleton PDESEngine + PDESWWT for FW. Use when the user asks about PDES code in this fork, multi-node FW, the multi command, /dev/shm/pdes_* setup during FW, or how parallel-qemu interacts with WormCacheQFlex for distributed snapshots.
---

This skill's content lives in `MULTI_NODE.md` next to this directory's `CLAUDE.md`. Read it now: [../../../MULTI_NODE.md](../../../MULTI_NODE.md).

That doc covers what parallel-qemu contributes to PDES, how it leans on the other submodules, and links to the sibling `MULTI_NODE.md` files (root + 4 sibling submodules) for the cross-cutting view.

## Incremental external snapshots (snapvm-external)

FW writes **incremental** per-sampling-unit checkpoints: a base memory image `<base>.mem/` + per-snapshot deltas `snapshot_<idx>.{loc,state.zstd}` (the distributed savevm persists each node's), plus the qcow2 internal snapshot entry. Writing happens here in FW (`migration/savevm.c` `incremental_snapshot_context`, `.mem/<index>` deltas). Loading them needs `-drive …,snapshot=on,tmp-snapshot-name=<name>` + `-loadvm <name>,on-demand`: `tmp-snapshot-name` (in `block.c` `bdrv_append_temp_snapshot` + `blockdev.c` option) loads the named snapshot's disk into the read-only base and **fakes a same-named empty snapshot in the transient overlay** so `bdrv_all_has_snapshot` passes; memory streams on demand. This option was originally **timing-fork-only**; it's been **ported here** so a fully-phantom node (parallel qemu in the timing phase) can load the per-idx checkpoints. **Fully gated on the option** — absent (every normal FW/load/boot) ⇒ stock behaviour, base image untouched.
