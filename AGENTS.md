# Agent Guide

Read [README.md](README.md) and the affected sources before changing code.

- Keep changes small and follow Linux kernel conventions. Write documentation in
  English; keep agent instructions here and navigation in READMEs.
- Preserve upstream provenance, copyright notices, GPL identifiers, and patch
  authorship. Inspect the current implementation and distinguish design proposals
  from implemented behavior.
- Trace behavioral changes through VFS operations, host/slave paths, transport,
  replay, and GC. Check visibility, log ordering, reference lifetimes, allocation
  accounting, and error propagation. Host/slave describe mount and protocol roles;
  establish the actual node topology separately.
- Validate affected configurations in a compatible Linux kernel tree, including
  URMA enabled and disabled where relevant. Use targeted functional, POSIX, stress,
  or cross-node tests. Record revisions, configuration, topology, commands, and
  outcomes; distinguish static checks, builds, harness tests, and real cross-node
  execution. Keep published results separate from local measurements.
- Use imperative commit subjects up to 72 characters, backticks for identifiers,
  and one logical change per commit. Keep private paths, credentials, build outputs,
  and generated runtime state out of tracked source.
