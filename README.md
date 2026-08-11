# XMFS

XMFS is an experimental Linux filesystem that exposes cross-node shared memory
through standard POSIX interfaces. It explores whether memory-semantic
interconnects such as CXL 3.0 and Huawei United Bus can make remote memory a
Linux storage tier.

The log-structured, copy-on-write design combines shared-memory, URMA, and hybrid
access paths with host/slave mount roles, log replay, and garbage collection.
Research focuses on metadata synchronization across kernels, software cache
coherence, transport integration, and capacity management and tiering.

## Development

[`xmfs/`](xmfs/) contains the filesystem sources, initially imported from the
[openEuler OLK-6.6 v8 posting](https://mailweb.openeuler.org/archives/list/kernel%40openeuler.org/message/BNGFSDNYHCFFK4ULUJ6XOLWTBXON24ZV/).
Building requires a compatible Linux kernel tree; URMA mode also requires its
UB/URMA dependencies. This repository currently contains no standalone build or
test harness.

## References

- [LPC 2026: XMFS — A Cross-node Express Memory File System](https://lpc.events/event/20/contributions/2516/)
- [LPC 2026 slides](https://lpc.events/event/20/contributions/2516/attachments/2149/5000/XMFS_LPC.pptx)

Licensed under **GPL-2.0-only**. See [COPYING](COPYING).
