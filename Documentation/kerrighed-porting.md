# Kerrighed Porting Guide
## Post-port optimization roadmap

The current Linux 3.10 Kerrighed work is primarily a correctness and
completeness restoration effort. Performance-oriented changes must not obscure
missing legacy semantics or make comparison with the original Kerrighed tree
more difficult.

The following areas have been identified during the port as promising
optimization targets for a future Kerrighed generation.

### Optimization rule: behavioral equivalence first

No optimization should be merged merely because it appears locally faster or
simpler.

Before optimizing a restored subsystem:

1. Recover the intended behavior from the legacy Kerrighed implementation.
2. Establish compile-time and runtime correctness of the restored path.
3. Add instrumentation capable of measuring the relevant distributed cost.
4. Establish a reproducible baseline.
5. Optimize the implementation.
6. Verify that distributed semantics remain equivalent.
7. Measure the result against the baseline.

In particular, successful compilation is not evidence that a distributed
optimization is correct.

### KRGRPC

KRGRPC is expected to be one of the most important optimization targets because
its behavior affects most higher-level Kerrighed distributed services.

Potential areas include batching acknowledgements where ordering semantics
permit it, reducing unnecessary receiver and sender wakeups, reducing lock
transitions around ordered receive queues, reducing allocation pressure, and
improving sustained high-throughput and low-memory flow-control behavior.

The restored low-memory and acknowledgement paths must be treated as protocol
semantics rather than merely implementation details. Any optimization therefore
requires multi-node fault and memory-pressure testing.

Useful measurements include:

    RPC messages/sec
    RPC bytes/sec
    RPC round-trip latency
    ACKs per message
    wakeups per RPC
    ordered-queue depth
    low-memory transitions
    allocation failures
    retransmission/recovery events

### KDDM object and set flushing

The restored KDDM set flushing implementation favors correctness when objects
may disappear while a set is being traversed.

The snapshot-and-flush strategy is robust, but potentially expensive for large
distributed sets because it can require additional memory and multiple passes.

Future alternatives worth investigating include chunked object-ID snapshots,
bounded-memory iteration, safe remove-aware iterators, batching object
relocation, avoiding repeated ownership lookups, and parallel flushing where
ordering permits it.

The lifetime guarantees currently obtained by separating object discovery from
destructive flushing must be preserved.

### KerMM reclaim and migratable pages

The restored migratable-page path deliberately favors correctness over reclaim
cost. Anonymous/shared mappings can require reverse-map traversal before a page
can be associated with a consistent Kerrighed memory object.

Possible future work includes caching distributed mapping metadata, reducing
repeated rmap traversal, maintaining cheaper page-to-KDDM lookup information,
batching migration candidates, improving migratable-LRU selection, and
improving destination-node selection.

Any cached mapping information must remain correct across fork, VMA mutation,
migration, exec, and mm teardown.

Useful counters include PG_migratable pages, migratable-LRU size, reclaim scans,
rmap walks, successful and failed distributed reclaims, page injections,
injection retries, and page relocation latency.

### KerMM memory injection and placement

Future versions should investigate cached placement information, batching
injected pages, reducing repeated PID-location queries, load-aware destination
selection, topology-aware placement, and avoiding repeated online-node scans.

Placement optimization must not break ownership, copyset, or KDDM consistency.

### Hotplug and cluster reconfiguration

Kerrighed hotplug deliberately performs strong distributed synchronization.
This is desirable while restoring correctness, but global barriers can become a
scalability limit as cluster size increases.

Future approaches worth evaluating include generation/epoch based membership
transitions, reducing global synchronization phases, separating independent
hotplug work, asynchronous preparation before the commit phase, batching
distributed cleanup, and reducing coordinator-transfer synchronization.

The restored notifier ordering must be considered part of the hotplug protocol.
Optimizations must preserve dependency ordering between membership, RPC,
barriers, KDDM, proc, KerMM, EPM, and coordinator management.

### Namespace and container lifecycle

The port currently preserves compatibility paths required while the complete
Kerrighed namespace/container lifecycle is being restored. After runtime
validation, duplicated bootstrap and fallback paths should be reviewed for
possible consolidation.

The long-term objective should be a single explicit lifecycle:

    namespace allocation
            |
            v
    distributed subsystem initialization
            |
            v
    cluster membership transition
            |
            v
    container becomes operational
            |
            v
    hotplug / migration / distributed execution
            |
            v
    ordered distributed teardown
            |
            v
    namespace destruction

### qrwlock and RCU opportunities

Several restored paths still require global task and subsystem synchronization.
During the port, some old tasklist-lock operations were found in code whose
target-kernel implementation had already moved to RCU.

Future auditing should classify lock sites as requiring write serialization,
requiring stable read traversal, suitable for RCU, suitable for per-object
locking, or suitable for lockless immutable state.

Replacing qrwlock operations with RCU must be based on object-lifetime analysis,
not merely on the apparent absence of writes in a function.

### Hashtable and distributed lookup scalability

Several Kerrighed subsystems perform global or namespace-wide hashtable walks.
Potential improvements include finer-grained locking, RCU-protected lookup
tables, per-node or per-namespace sharding, cached ownership/location
information, generation counters for cache invalidation, and reducing full-table
traversal during cleanup.

### Instrumentation before optimization

A future Kerrighed should expose enough internal telemetry that optimization
decisions can be based on measurements rather than intuition.

Instrumentation should cover KRGRPC message rates, bytes, latency, queue depths,
ACK behavior and low-memory transitions; KDDM set/object counts, ownership
changes, flushes and relocation latency; KerMM migratable pages, reclaim,
injection and migration; hotplug phase/barrier/coordinator latency; and
EPM/DVFS/FAF remote file and checkpoint/restart activity.

Where practical, counters should be per-node as well as cluster-wide.

### Benchmark strategy

Optimization work should use repeatable workloads rather than isolated
microbenchmarks alone. Useful workloads include RPC ping-pong and streaming,
large KDDM set creation/destruction, distributed anonymous-memory pressure,
fork with shared anonymous memory, process migration under memory pressure,
node addition/removal, coordinator relocation, distributed file access,
checkpoint/restart, and mixed process/memory/file migration.

Tests should be repeated with increasing cluster sizes so that an optimization

This document records the Kerrighed port used by PhiWeave OS and serves as
a compatibility reference for future kernel ports.

The primary goals are:

- document kernel API changes required by Kerrighed,
- record semantic assumptions and invariants which are not visible from
  compiler errors alone,
- preserve runtime lessons discovered during bring-up,
- provide a checklist for future ports to newer Linux kernels.

## 1. Reference platform

Current reference kernel:

- Linux: CentOS 7-derived 3.10 kernel
- Architecture: x86_64
- Kerrighed development support: enabled
- KDDM: enabled
- KerMM: enabled

Important KerMM configuration constraints:

    CONFIG_KRG_KDDM=y
    CONFIG_KRG_MM=y

    # CONFIG_MEM_SOFT_DIRTY is not set
    # CONFIG_TRACK_DIRTY_PAGES is not set
    # CONFIG_COMPACTION is not set
    # CONFIG_KSM is not set
    # CONFIG_TRANSPARENT_HUGEPAGE is not set

## 2. Build reference

Discovery build:

    make \
      ARCH=x86_64 \
      CROSS_COMPILE=x86_64-linux-gnu- \
      KCFLAGS="-fmax-errors=3 -fdiagnostics-color=always" \
      -k -j4 \
      bzImage

Validation build:

    make \
      ARCH=x86_64 \
      CROSS_COMPILE=x86_64-linux-gnu- \
      KCFLAGS="-fmax-errors=3 -fdiagnostics-color=always" \
      -j4 \
      bzImage

Do not use `make clean` during incremental porting.

## 3. Porting milestones

### Communication and hotplug

Status:

- single-node Kerrighed boot: working
- two-node discovery: working
- two-node join: working

### KDDM

Status:

- compile: working
- link: working
- initialization: working
- two-node runtime: working

### KerMM

Status:

- compile: working
- link: working
- kernel boot: working
- two-node Kerrighed join with CONFIG_KRG_MM=y: working
- distributed MM runtime semantics: pending

Runtime tests still required:

- process migration
- memory access after migration
- remote page fault
- fork/COW
- stress testing

## 4. Kernel compatibility map

This section maps Kerrighed assumptions to the current kernel APIs.

### Highmem API

Old Kerrighed code used legacy multi-argument atomic mapping interfaces.

Current port uses:

    kmap_atomic(page)
    kunmap_atomic(addr)

Affected KerMM memory linker code was converted to the current API.

### RSS accounting

Legacy fields such as:

    anon_rss
    file_rss

were replaced by MM counters:

    MM_ANONPAGES
    MM_FILEPAGES

Audit all future ports for changes to MM RSS accounting helpers.

### Page locking

Legacy:

    TestSetPageLocked(page)

was replaced where appropriate with:

    trylock_page(page)

Do not mechanically replace lock operations without checking the original
locking semantics.

### VMA flags

KerMM widens `vm_area_struct.vm_flags` under CONFIG_KRG_MM.

The widened type must be propagated consistently through local variables and
helper interfaces such as madvise/KSM/THP compatibility paths.

Do not hide width mismatches using pointer casts.

### anon_vma

Fake VMA initialization must initialize:

    INIT_LIST_HEAD(&vma->anon_vma_chain)

The old Kerrighed manual anon_vma linkage uses data structures which no
longer match the current kernel.

The current port leaves:

    vma->anon_vma = NULL

and relies on:

    anon_vma_prepare(vma)

when required.

WARNING:

`anon_vma_prepare()` may sleep and has mmap locking requirements. Before
aggressive distributed-MM testing, audit the locking context of every KerMM
callback which may invoke it.

### Page fault / COW path

The KerMM VM_KDDM COW path differs from the normal Linux COW path.

Important invariant:

When VM_KDDM is set, the local COW page may intentionally not be allocated.

Therefore the `cow_page` argument passed into the file fault path must have
a defined value (`NULL`) when no local page exists.

Another critical invariant:

`__do_fault()` may return

    VM_FAULT_ERROR
    VM_FAULT_NOPAGE
    VM_FAULT_RETRY
    VM_FAULT_DONE_COW

without producing a usable `fault_page`.

KerMM code MUST validate these return states before dereferencing
`fault_page`.

This was discovered during the warning audit before KerMM runtime testing.

### VDSO import

x86_64 and compat VDSO code originally provided duplicate public Kerrighed
hooks.

The current design uses the x86_64 VDSO implementation as the public
dispatcher.

For IA32 tasks it dispatches to dedicated 32-bit helpers.

Keep the distinction between:

- native x86_64
- IA32 compat
- X32

when porting this code again.

### mm_struct import

Architecture MM context required compatibility state to be imported,
including IA32 compatibility information.

Obsolete fields from older kernels must not simply be copied back into the
new structure.

Review each `mm_struct` field semantically on every kernel upgrade.

## 5. Known semantic audit debt

These areas compiled successfully but still require runtime/locking review.

### Fake VMA / anon_vma locking

Some KerMM paths construct or locate VMAs outside ordinary userspace mmap
flows.

Do not blindly add mmap locks: callbacks may already execute under locking
and an extra lock could deadlock.

Audit caller context first.

### Fake mm_struct initialization

KerMM contains code which copies an existing mm_struct and subsequently
reinitializes selected state.

This is structurally risky because mm_struct evolves significantly between
kernel versions.

Future ports should explicitly review every copied field.

## 6. Warning policy

During subsystem bring-up:

- existing legacy/toolchain warnings may temporarily remain,
- new warnings caused by the port must be investigated.

Before stabilization:

- zero compile errors,
- zero meaningful warnings,
- no broad `-Wno-*` suppression.

Warnings in MM/page-fault/VMA/KDDM paths are treated as potential latent
runtime bugs until proven otherwise.

## 7. Future kernel port checklist

For each future Linux version, audit at minimum:

- mm_struct
- vm_area_struct
- vm_flags type and flag allocation
- anon_vma
- page fault API
- COW handling
- vm_fault structure
- PTE/PMD helpers
- page locking
- page reference APIs
- RSS accounting
- rmap
- mmap locking
- VDSO architecture state
- fork/clone MM handling
- KDDM PTE encoding
- memory migration
- highmem APIs
- procfs APIs
- TIPC/network transport APIs

Do not consider a subsystem port complete after compile/link alone.
Each subsystem requires runtime validation.

## 8. Runtime validation matrix

| Test | Single node | Two nodes |
|---|---:|---:|
| Kernel boot | PASS | PASS |
| Kerrighed initialization | PASS | PASS |
| Node join | N/A | PASS |
| KDDM initialization | PASS | PASS |
| KerMM initialization | PASS | PASS |
| Process migration | TODO | TODO |
| Memory after migration | TODO | TODO |
| Remote page fault | TODO | TODO |
| fork/COW | TODO | TODO |
| Stress | TODO | TODO |


<!-- KERRIGHED-PORTING-GUIDE-BEGIN -->

# Kerrighed Kernel Porting Guide

This document records the Kerrighed kernel port used by PhiWeave OS.

It is intended to serve as:

- a porting journal for the current Linux 3.10 work,
- a compatibility reference for future Linux 4.x and newer ports,
- a record of semantic assumptions which compiler errors alone cannot reveal,
- a runtime validation checklist,
- a collection of examples showing legacy Kerrighed code and its ported form.

The long-term goal is to make future Kerrighed kernel ports reproducible
instead of requiring the same kernel archaeology again.

## 1. Reference platform

Current reference kernel:

    Linux 3.10.0+
    Architecture: x86_64
    Kerrighed development mode: enabled
    KDDM: enabled
    KerMM: enabled

Relevant configuration:

    CONFIG_KERRIGHED_DEVEL=y
    CONFIG_KRG_KDDM=y
    CONFIG_KRG_MM=y

KerMM currently requires:

    # CONFIG_MEM_SOFT_DIRTY is not set
    # CONFIG_TRACK_DIRTY_PAGES is not set
    # CONFIG_COMPACTION is not set
    # CONFIG_KSM is not set
    # CONFIG_TRANSPARENT_HUGEPAGE is not set

## 2. Build reference

Discovery build:

```sh
make \\
  ARCH=x86_64 \\
  CROSS_COMPILE=x86_64-linux-gnu- \\
  KCFLAGS="-fmax-errors=3 -fdiagnostics-color=always" \\
  -k -j4 \\
  bzImage
```

Full validation build:

```sh
make \\
  ARCH=x86_64 \\
  CROSS_COMPILE=x86_64-linux-gnu- \\
  KCFLAGS="-fmax-errors=3 -fdiagnostics-color=always" \\
  -j4 \\
  bzImage
```

Incremental porting deliberately avoids `make clean`.

## 3. Current milestone

The current KerMM-enabled kernel has reached the following state:

| Test | Status |
|---|---|
| x86_64 compile | PASS |
| bzImage link | PASS |
| Kernel boot | PASS |
| Kerrighed initialization | PASS |
| KDDM initialization | PASS |
| KerMM initialization | PASS |
| TIPC two-node link | PASS |
| Kerrighed two-node join | PASS |
| Explicit process migration | PENDING |
| Memory access after migration | PENDING |
| Remote page fault | PENDING |
| fork/COW distributed test | PENDING |
| Stress testing | PENDING |

Reference boot milestone:

    Linux 3.10.0+ #18

Observed initialization:

    KDDM initialisation : start
    KDDM initialisation done
    KerMM initialisation : start
    KerMM initialisation done
    Kerrighed... loaded!

Observed two-node operation:

    tipc: Established link <...>
    Kerrighed is running on 2 nodes

This proves initialization and cluster join. It does NOT yet prove
distributed KerMM page-fault or COW semantics.

## 4. Development configuration and subsystem selection

`CONFIG_KERRIGHED_DEVEL=y` prevents Kerrighed from automatically selecting
several higher-level subsystems.

The Kerrighed Kconfig contains:

```text
select KRG_CAP   if !KERRIGHED_DEVEL
select KRG_PROC  if !KERRIGHED_DEVEL
select KRG_EPM   if !KERRIGHED_DEVEL
select KRG_SCHED if !KERRIGHED_DEVEL
```

Development builds must therefore explicitly enable the subsystems being
ported and tested.

### Manual process migration

The current Kconfig defines:

```text
KRG_PROC:
    depends on KERRIGHED && KRG_KDDM

KRG_CAP:
    depends on KERRIGHED

KRG_EPM:
    depends on KERRIGHED && KRG_PROC && KRG_CAP

KRG_SCHED:
    depends on KRG_EPM
```

The minimum intended stack for the next explicit migration test is:

```text
CONFIG_KRG_KDDM=y
CONFIG_KRG_MM=y
CONFIG_KRG_PROC=y
CONFIG_KRG_CAP=y
CONFIG_KRG_EPM=y
```

`KRG_SCHED` is not required merely to request an explicit process migration.
Scheduler and automatic load-balancing support should be ported separately.

An older commented EPM dependency also exists:

```text
KERRIGHED && KRG_PROC && KRG_CAP && KRG_MM &&
(KRG_DVFS || KRG_FAF)
```

This is an important historical clue.

Future ports should verify the real runtime relationship between EPM and
these subsystems rather than assuming the reduced Kconfig dependency fully
describes the implementation.

## 5. Kernel compatibility map

### 5.1 Highmem API

Legacy Kerrighed code used the old mapping-slot API:

```c
addr = kmap_atomic(page, KM_USER0);

/* access page */

kunmap_atomic(addr, KM_USER0);
```

The current kernel uses:

```c
addr = kmap_atomic(page);

/* access page */

kunmap_atomic(addr);
```

The slot argument disappeared from the API.

Affected code includes KerMM memory linker paths.

### 5.2 Page locking

Legacy Kerrighed code contained patterns such as:

```c
BUG_ON(TestSetPageLocked(page));
```

The current port uses:

```c
BUG_ON(!trylock_page(page));
```

This is not a textual rename.

The return semantics differ:

- `TestSetPageLocked()` reports whether the lock bit was already set.
- `trylock_page()` reports whether the lock was successfully acquired.

A mechanical replacement without checking the condition can therefore
invert the original behavior.

### 5.3 RSS accounting

Legacy code used old RSS state which no longer matches the current MM API.

Conceptually, old code referred to accounting such as:

```c
mm->anon_rss
mm->file_rss
```

The current port uses MM counters such as:

```c
MM_ANONPAGES
MM_FILEPAGES
```

Future ports must review RSS accounting semantically instead of restoring
removed `mm_struct` fields.

### 5.4 VMA flag width

KerMM extends the available VMA flag space.

This port widens `vm_flags` under `CONFIG_KRG_MM`, so the type must remain
consistent through every helper interface.

Problematic example:

```c
unsigned long new_flags = vma->vm_flags;
hugepage_madvise(vma, &new_flags, behavior);
```

KerMM-aware form:

```c
#ifdef CONFIG_KRG_MM
unsigned long long new_flags = vma->vm_flags;
#else
unsigned long new_flags = vma->vm_flags;
#endif
```

Interfaces receiving a pointer to the value must use the same width.

For example:

```c
#ifdef CONFIG_KRG_MM
int ksm_madvise(..., unsigned long long *vm_flags);
#else
int ksm_madvise(..., unsigned long *vm_flags);
#endif
```

Do not hide width mismatches using pointer casts.

### 5.5 KerMM COW fault path

The warning audit exposed a real latent KerMM bug.

A simplified problematic pattern was:

```c
struct page *fault_page, *new_page;

#ifdef CONFIG_KRG_MM
if (!(vma->vm_flags & VM_KDDM)) {
#endif
        new_page = alloc_page_vma(...);
#ifdef CONFIG_KRG_MM
}
#endif

ret = __do_fault(..., new_page, &fault_page, ...);

#ifdef CONFIG_KRG_MM
if ((vma->vm_flags & VM_KDDM) &&
    (!fault_page->mapping || PageAnon(fault_page))) {
        ...
}
#endif
```

For a `VM_KDDM` VMA, `new_page` could remain uninitialized.

More importantly, `fault_page` could be dereferenced before checking whether
`__do_fault()` returned a state in which a valid page was produced.

The port now initializes the optional COW page explicitly:

```c
struct page *fault_page, *new_page = NULL;
```

The fault result is then validated before `fault_page` is dereferenced:

```c
ret = __do_fault(...);

if (unlikely(ret & (VM_FAULT_ERROR |
                    VM_FAULT_NOPAGE |
                    VM_FAULT_RETRY))) {
#ifdef CONFIG_KRG_MM
        if (vma->vm_flags & VM_KDDM)
                return ret;
#endif
        goto uncharge_out;
}

if (ret & VM_FAULT_DONE_COW)
        return ret;

#ifdef CONFIG_KRG_MM
if ((vma->vm_flags & VM_KDDM) &&
    (!fault_page->mapping || PageAnon(fault_page))) {
        ...
}
#endif
```

#### Porting invariant

Never dereference `fault_page` until the return state from `__do_fault()` has
been validated.

For `VM_KDDM`, absence of a locally allocated COW page is valid and must be
represented by a defined value such as `NULL`.

#### How the bug was discovered

Modern GCC reported:

```text
warning: 'new_page' may be used uninitialized
```

This warning was not cosmetic.

Following the warning through the control flow also exposed the unsafe
`fault_page` dereference ordering.

Warnings in MM fault paths must therefore be treated as possible semantic
bugs until audited.

### 5.6 Read and shared fault paths

Old Kerrighed fault code referred to page variables which no longer matched
the current `vm_fault` implementation.

The current port tracks the actual page returned by the fault helper and
only uses it after validating the helper result.

The same rule applies to:

- read faults,
- shared faults,
- COW faults,
- KDDM PTE insertion.

Future ports should compare the complete fault state machine rather than
only updating function signatures.

### 5.7 PTE allocation API

An older call form:

```c
__pte_alloc(mm, pmd, address)
```

became:

```c
__pte_alloc(mm, NULL, pmd, address)
```

Additional arguments must be checked against the semantics of the target
kernel rather than filled arbitrarily merely to satisfy compilation.

### 5.8 Page-table MMU update

The current fault path uses the page-table pointer expected by the kernel:

```c
update_mmu_cache(vma, address, page_table);
```

### 5.9 VDSO import

The x86_64 and compatibility VDSO code originally produced conflicting
public Kerrighed import hooks.

The current design has one public x86_64 dispatcher with dedicated 32-bit
helpers.

Conceptually:

```c
void import_vdso_context(...)
{
#ifdef CONFIG_COMPAT
        if (/* IA32 task */) {
                import_vdso32_context(...);
                return;
        }
#endif

        /* native x86_64 handling */
}
```

Future ports must preserve the distinction between native x86_64, IA32
compatibility tasks and X32.

### 5.10 mm_struct reinitialization

Imported or fake MM structures require semantic review whenever the kernel
changes.

Examples already encountered include:

```c
cpumask_clear(mm_cpumask(mm));
atomic_long_set(&mm->nr_ptes, 0);
```

Obsolete fields must be removed instead of recreated merely to satisfy old
Kerrighed code.

## 6. Fake VMA and anon_vma handling

Fake VMA initialization now includes:

```c
INIT_LIST_HEAD(&vma->anon_vma_chain);
vma->anon_vma = NULL;
```

The current design relies on:

```c
anon_vma_prepare(vma);
```

when an anon_vma is required.

### Audit warning

`anon_vma_prepare()` can sleep and has locking requirements.

Some KerMM paths create or locate VMAs outside normal userspace mmap flows.
Do not blindly add `mmap_sem` acquisition around these paths: a callback may
already execute under a lock and another acquisition could deadlock.

The caller and locking context must be audited first.

## 7. Fake mm_struct audit debt

KerMM contains code which copies an existing `mm_struct` and subsequently
reinitializes selected members.

This is sensitive to kernel version changes because `mm_struct` evolves
substantially.

For future ports:

1. enumerate every copied field,
2. classify ownership and reference-count semantics,
3. identify lists, locks and counters which must be reinitialized,
4. verify architecture state separately,
5. test teardown as carefully as creation.

A successful compile does not prove that this operation is safe.

## 8. Warning policy

During initial subsystem bring-up, unrelated legacy compiler warnings may
temporarily remain.

However:

- warnings introduced by the port must be investigated,
- MM/VMA/page-fault/rmap/KDDM warnings are treated as potential runtime bugs,
- broad `-Wno-*` suppression is not used,
- final stabilization aims for zero warnings.

A warning audit before the first KerMM runtime test already found a real COW
fault-path bug, validating this policy.

## 9. Runtime validation strategy

KerMM should be validated incrementally.

### Stage 1: platform

```text
kernel boot
Kerrighed initialization
KDDM initialization
KerMM initialization
TIPC link
two-node cluster join
```

Status: PASS.

### Stage 2: explicit process migration

Enable the minimum process-management stack and migrate one simple process
between Kerrighed nodes.

Use a process with a small and deterministic memory state.

### Stage 3: memory after migration

Verify that writable anonymous pages retain their expected contents after
the process changes node.

### Stage 4: remote page-fault behavior

Force accesses which require KerMM/KDDM page resolution instead of only
touching already-local pages.

### Stage 5: fork/COW

Test:

```text
fork
parent writes
child writes
page divergence
migration
continued data integrity
```

This specifically exercises the fault path which required semantic repair
during the port.

### Stage 6: stress

Only after deterministic tests pass should testing expand to:

- larger address spaces,
- repeated migrations,
- multiple processes,
- concurrent COW,
- node churn,
- longer-running stress tests.

## 9.1 KRGRPC completeness audit

The original Linux 3.10 bring-up validated KRGRPC sufficiently for:

- kernel boot,
- TIPC initialization,
- two-node discovery,
- two-node Kerrighed join.

That milestone did not prove full functional equivalence with the original
Kerrighed KRGRPC implementation.

A later old-tree versus new-tree completeness audit found that the file set in
`net/krgrpc/` is still present, but substantial behavior had been removed or
changed inside `rpc.c`, `rpclayer.c`, `comlayer.c` and `synchro.c`.

The audit therefore distinguishes:

- behavior intentionally replaced by the Linux 3.10 global KRGRPC design,
- original invariants which must be restored,
- functionality which belongs to higher-level subsystems and must be restored
  together with those subsystems.

### Restored KRGRPC core semantics

The following behavior has now been restored and targeted-build validated.

#### RPC synchronization

`net/krgrpc/synchro.c` had semantic regressions unrelated to a kernel API
change.

The original DEAD flag operation:

```c
flags |= __RPC_SYNCHRO_DEAD;
```

had become an `&=` operation. Original error handling for allocation,
radix-tree insertion and missing entries was also changed.

The original synchronization semantics were restored.

Validation:

```text
net/krgrpc/synchro.o    PASS
net/krgrpc/             PASS
```

#### RPC descriptor wait/return semantics

The Linux 3.10 port lost part of the descriptor completion semantics.

`rpc_wait_return()` now checks both pending unexpected data and
`RPC_FLAGS_CLOSED`, preventing a waiter from sleeping indefinitely after the
descriptor has already closed.

The non-blocking `rpc_check_return()` behavior was also restored.

Validation:

```text
net/krgrpc/rpclayer.o   PASS
net/krgrpc/             PASS
```

#### TX memory accounting

The original `rpc_consumed_bytes()` accounting was restored around
`rpc_tx_elem` allocation and final release.

This accounting is required by the original KerMM low-memory integration and
is retained even before the higher-level memory-pressure path is restored.

#### RX OOM/backpressure

The bootstrap port converted several RX allocation failures into `BUG()`.
The original Kerrighed behavior instead preserved the packet and retried it
when memory became available.

The Linux 3.10 global per-node ordering design is retained, but its OOM
semantics are now restored:

```text
next ordered packet
        |
        +-- processed --> advance receive sequence
        |
        `-- -ENOMEM --> keep packet at queue head
                        do not advance sequence
                        retry later
```

Validation:

```text
net/krgrpc/comlayer.o   PASS
net/krgrpc/             PASS
```

#### Network-device lifecycle

Per-device and all-device KRGRPC bearer enable/disable entry points were
restored for later hotplug integration.

The old `rpc_disable_all()` excluded `RPC_CONNECT` and `RPC_CLOSE`. Those RPC
IDs belonged to the removed per-communicator connection handshake and do not
exist in the current global KRGRPC design, so the Linux 3.10 implementation
disables the currently defined RPC service IDs instead of recreating obsolete
control IDs.

Targeted `rpc.o`, `comlayer.o` and full `net/krgrpc/` builds pass.

### KRGRPC work still pending

KRGRPC is not yet marked complete.

The following areas remain intentionally deferred until their owning
subsystem is audited:

- connection and connect/close-mask lifecycle: restore with hotplug/namespace,
- low-memory mode: restore with KerMM injection,
- RPC cancellation and forwarding: restore with EPM,
- node removal and failure recovery: audit with hotplug and KDDM teardown.

The original `rpc_communicator` and per-connection architecture must not be
restored mechanically. The Linux 3.10 port uses a global communication model,
so each missing original invariant must be mapped onto that design.

<!-- KERRIGHED-COMPLETENESS-RESTORE-2026-BEGIN -->

## 9.2 Completeness restoration after the initial Linux 3.10 bring-up

The first Linux 3.10 port reached important compile and runtime milestones, but
later old-tree versus new-tree audits showed that those milestones did not imply
full functional equivalence with the original Kerrighed tree.

The current porting rule is therefore:

> A subsystem is not complete merely because it builds, links, boots, or passes
> a basic two-node test. Every subsystem previously considered ported must be
> compared against the original tree for missing files, functions, kernel hooks,
> lifecycle transitions, and semantic invariants.

Each difference is classified as one of:

- **PORTED** - original behavior is preserved on the target kernel API.
- **REDESIGNED** - behavior is preserved through an intentional target-kernel
  architecture change.
- **INCOMPLETE** - original Kerrighed functionality is missing.
- **DEFERRED** - the missing behavior belongs to another subsystem which must
  be restored first.

This completeness audit is required before progressing to higher-level
subsystems such as EPM and the distributed scheduler.

### 9.2.1 Build validation strategy

Small changes are validated with a targeted object build first:

```sh
make -j4 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- \
  KCFLAGS="-fdiagnostics-color=always" \
  path/to/object.o \
  2>&1 | tee build-<subsystem>-<task>.log
```

A restored subsystem is then built as a directory target:

```sh
make -j4 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- \
  KCFLAGS="-fdiagnostics-color=always" \
  kerrighed/mm/ \
  2>&1 | tee build-kermm-subsystem-check.log
```

Full integration sweeps use `-k` so independent failures can be grouped by
their common root cause:

```sh
make -k -j4 ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu- bzImage \
  KCFLAGS="-fdiagnostics-color=always" \
  2>&1 | tee build-port-completeness-error-sweep.log
```

Build logs are named after the subsystem or porting task so that historical
results remain attributable.

### 9.2.2 Effective-current execution-context restoration

The original Kerrighed EPM model redirects `current` through
`task_struct::effective_current`. The initial Linux 3.10 port retained only part
of this mechanism.

The original model relies on `krg_current` being an assignable lvalue:

```c
#define krg_current (get_current()->effective_current)
```

while code which may schedule temporarily exposes the physical worker task:

```c
#define krg_current_save(tmp) do { \
        (tmp) = krg_current;        \
        krg_current = NULL;         \
} while (0)

#define krg_current_restore(tmp) do { \
        krg_current = (tmp);          \
} while (0)
```

On the target kernel, `current.h` may be parsed before `struct task_struct` is
complete. Direct dereference from that header therefore cannot be retained.
The port uses accessors while preserving `krg_current` as an lvalue:

```c
struct task_struct *krg_get_current(void);
struct task_struct **krg_current_ptr(void);

#define krg_current (*krg_current_ptr())
#define current krg_get_current()
```

The original save/restore boundaries were restored around scheduler and MM
sleep paths, with additional protection around Linux 3.10
`sched_submit_work()`.

This is only the execution-context foundation. Full EPM process
reconstruction, migration, remote clone, signal/sighand state and children
tracking remain separate EPM completeness work.

### 9.2.3 KRGRPC completeness restoration

The initial communication milestone proved TIPC discovery and two-node join,
not full KRGRPC equivalence.

The audit restored several independent core invariants.

#### Synchronization DEAD flag

A bootstrap regression changed:

```c
flags |= __RPC_SYNCHRO_DEAD;
```

into an `&=` operation, changing the state-machine semantics. The original
set-bit behavior and error handling were restored.

#### Descriptor completion

`rpc_wait_return()` must stop waiting when a descriptor is closed, not only
when unexpected data exists. The non-blocking `rpc_check_return()` behavior was
also restored.

#### TX memory accounting

The original RPC payload accounting was restored:

```c
consumed_bytes_add(size);
...
consumed_bytes_sub(elem->iov[1].iov_len);
```

This later became a dependency of KerMM low-memory flow control.

#### Ordered RX OOM handling

The bootstrap port converted receive allocation failures into `BUG()`. The
restored model keeps the expected packet at the head of the per-node ordering
queue and retries it later without advancing the receive sequence:

```text
packet N
   |
   +-- success  -> advance sequence -> process N+1
   |
   `-- -ENOMEM -> keep N queued
                  do not advance sequence
                  retry later
```

The current global per-node ordering design is retained; the old
`rpc_communicator` object is not restored mechanically.

#### Low-memory flow control

The original per-connection low-memory throttling was adapted to the global
KRGRPC model by maintaining receive thresholds per remote node.

### 9.2.4 Hotplug and Kerrighed namespace restoration

The initial two-node autostart path had removed most of the original Kerrighed
namespace and hotplug orchestration.

The completeness audit restored:

- `struct krg_namespace` lifetime and reference counting,
- `nsproxy->krg_ns`,
- cluster PID namespace linkage,
- `task_struct::create_krg_ns`,
- namespace creation across fork/unshare,
- cluster creator service,
- cluster barrier infrastructure,
- hotplug request context and serialized start/run/finish requests,
- hotplug coordinator ownership transfer,
- cluster container helper lifecycle,
- HOTPLUG_READY container handshake.

The old namespace contained a per-namespace `rpc_communicator`. That field was
not restored because the Linux 3.10 port intentionally uses a global KRGRPC
communication domain.

Example transport adaptation:

```c
/* Original Kerrighed */
rpc_begin(HOTPLUG_START_REQ, ctx->ns->rpc_comm, coordinator);

/* Linux 3.10 port */
rpc_begin(HOTPLUG_START_REQ, coordinator);
```

### 9.2.5 Hotplug notifier priority invariant

The old-tree audit found that notifier priorities in the bootstrap port were
numerically reversed while Linux notifier chains still execute higher
priorities first.

The relative ordering was restored before reconnecting KerMM:

```text
MEMBERSHIP_PRESENT
RPC
BARRIER
KDDM
PROCFS
MM
EPM
HOTPLUG_COORDINATOR
MEMBERSHIP_ONLINE
MEMBERSHIP_POSSIBLE
```

### 9.2.6 KDDM set-wide flush restoration

KerMM hotplug exposed another missing KDDM primitive:
`_kddm_flush_set()` / `kddm_flush_set()`.

The original implementation relied on a safe iterator ABI which no longer
matches the target tree. The port preserves its semantics using a two-phase
operation:

```text
walk KDDM set under current iterator locking
        |
        v
snapshot object IDs
        |
        v
release iterator/table locks
        |
        v
flush each object with normal path/object locking
```

### 9.2.7 KerMM completeness restoration

The original KerMM bring-up proved initialization and two-node join, but the
old-tree audit found several entire missing layers.

#### Mobility and x86 LDT

Linux 3.10 stores the LDT in a container structure:

```c
struct ldt_struct {
        struct desc_struct *entries;
        unsigned int size;
};
```

The port serializes the actual descriptor entries:

```c
ghost_write(ghost, ldt->entries,
            ldt->size * LDT_ENTRY_SIZE);
```

#### KerMM hotplug lifecycle

Anonymous-memory KDDM sets again keep their original PID/TGID placement hints:

```c
private.last_pid = task_pid_knr(tsk);
private.last_tgid = task_tgid_knr(tsk);
```

#### Migratable-page LRU model

The original dedicated migratable page model was restored:

```text
PG_migratable
LRU_INACTIVE_MIGR
LRU_ACTIVE_MIGR
NR_INACTIVE_MIGR
NR_ACTIVE_MIGR
```

#### Memory injection and reclaim

For fork/shared anonymous pages, reclaim preserves the relationship between the
remaining virtual mapping and its KDDM object:

```text
VMA
 -> vma->vm_mm
 -> mm->anon_vma_kddm_set
 -> virtual address >> PAGE_SHIFT
 -> KDDM object ID
```

Only then is `_kddm_flush_object()` invoked.

#### Zone low-watermark API

```c
/* Original */
low_mem_limit += zone->pages_low;

/* Target kernel */
low_mem_limit += low_wmark_pages(zone);
```

#### Tasklet API

```c
#include <linux/interrupt.h>
```

is used instead of the obsolete standalone `linux/tasklet.h`.

### 9.2.8 Distributed VMA mutation propagation

A second KerMM completeness audit found that the bootstrap port propagated
`munmap` but had lost distributed updates for:

- `mmap`,
- `mremap`,
- `brk`,
- stack expansion,
- `mprotect`.

Remote application borrows the distributed `mm` with `use_mm()` /
`unuse_mm()` and reuses the target kernel's own MM operations.

A task-local guard prevents remotely applied mutations from generating another
RPC notification:

```c
current->krg_mm_remote_apply = 1;
use_mm(mm);

/* Apply current Linux MM operation. */

unuse_mm(mm);
current->krg_mm_remote_apply = 0;
```

The existing `kh_do_mmap` callback remains a local KerMM linker hook and is
kept separate from distributed mmap notification so that operations such as
`brk` do not accidentally emit two mutation RPCs.

### 9.2.9 Remaining KerMM audit debt

KerMM must not yet be labelled fully complete.

Remaining areas identified by the second completeness audit include:

- memcg accounting for the dedicated migratable LRU classes,
- `/proc/meminfo` visibility for active/inactive migratable pages,
- OCFS2 `vm_ops` mobility/export integration,
- runtime validation of distributed VMA mutation propagation,
- runtime memory-pressure/injection tests,
- node-removal memory relocation,
- EPM-owned checkpoint/restart MM paths.

### 9.2.10 Current subsystem status

At this checkpoint:

```text
KRGRPC core semantics                 restored / targeted-build validated
Kerrighed namespace core             restored / targeted-build validated
cluster barrier                       restored / targeted-build validated
hotplug request engine               restored / targeted-build validated
hotplug coordinator                  restored / targeted-build validated
cluster container core/start         restored / targeted-build validated
KDDM set-wide flush                  restored / targeted-build validated
KerMM mobility/LDT                   restored / targeted-build validated
KerMM hotplug                        restored / targeted-build validated
KerMM migratable LRU                 restored / targeted-build validated
KerMM injection/reclaim              restored / targeted-build validated
KerMM VMA mutation propagation       targeted-build validated

full bzImage                          integration sweep in progress
EPM process reconstruction           incomplete
distributed scheduler               incomplete
```

Targeted build validation is not runtime validation and is not subsystem
completeness by itself.

<!-- KERRIGHED-COMPLETENESS-RESTORE-2026-END -->

## 10. Future kernel port checklist

At minimum, audit all of the following for every target kernel:

- `mm_struct`
- `vm_area_struct`
- VMA flag allocation and width
- `anon_vma`
- mmap locking
- `vm_fault`
- read/shared/COW fault paths
- PTE and PMD APIs
- page locking
- page reference counting
- rmap
- RSS accounting
- page flags
- page migration
- highmem APIs
- fork/clone MM handling
- VDSO architecture context
- KDDM PTE encoding
- process migration/EPM
- procfs APIs
- TIPC/network transport
- scheduler integration

Every compatibility change should preferably record:

1. the legacy code,
2. the target-kernel equivalent,
3. why the change is necessary,
4. any semantic invariant,
5. how it was tested,
6. the files/functions affected.

## 11. Long-term modernization strategy

The Linux 3.10 port should become the reference point for later ports.

A possible progression is:

```text
Linux 3.10
    |
    v
Linux 4.x reference port
    |
    v
later LTS kernels
    |
    v
current Linux
```

Where repeated compatibility differences become clear, they should
eventually move behind narrowly scoped Kerrighed compatibility helpers
instead of adding version-dependent code throughout the distributed kernel
subsystems.

The goal is for future Kerrighed ports to become controlled compatibility
work rather than repeated reverse engineering.

<!-- KERRIGHED-PORTING-GUIDE-END -->
