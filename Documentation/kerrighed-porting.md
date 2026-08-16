# Kerrighed Porting Guide

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
