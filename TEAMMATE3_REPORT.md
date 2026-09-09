# Teammate 3 — Benchmarking & Report

## 1. Benchmark Environment

- Modified xv6-riscv with unified page-fault handling.
- QEMU memory: 128 MB
- SMP: 3 harts
- Tests were executed on the modified xv6 build from the `teammate3-handover` branch.

## 2. COW Benchmark

Command:

    cowtest

Output:

    cowtest: 200 pages resident, forking 4 children...
    cowtest: done.
    cowtest: cow faults triggered = 20

Result:

The test triggered approximately 20 COW faults, corresponding to 4 children modifying 5 pages each. This demonstrates that pages are initially shared and private copies are created when a write occurs.

## 3. Memory Snapshot After COW

Command:

    memtop

Observed:

    frames   : total=32768 free=32332 allocated=198 cap=0
    faults   : zero=0 file=8 cow=27
    replace  : evictions=0 reloads=0

The system had no page-replacement activity because no artificial memory cap was imposed.

## 4. Page Replacement Benchmark

Command:

    stresstest 40 6

Configuration:

- Memory cap: 40 frames
- Concurrent workers: 6
- Exec rounds per worker: 15

Observed:

    file-faults = 303
    evictions = 212
    reloads = 158

The test produced substantial eviction and reload activity, demonstrating that Module 3 page replacement is triggered under memory pressure.

## 5. Memory Snapshot After Stress Test

Command:

    memtop

Observed:

    frames   : total=32768 free=32332 allocated=198 cap=0
    faults   : zero=0 file=313 cow=46
    replace  : evictions=212 reloads=158

The cumulative counters confirm that page eviction and subsequent page reload occurred during the stress workload.

## 6. Stock xv6 Baseline

A separate unmodified xv6-riscv checkout was built successfully.

Command:

    forktest

Observed:

    fork test
    fork test OK

This confirms that the baseline xv6 build and basic fork functionality operate correctly.

A direct performance comparison was not performed because the stock xv6 implementation does not provide the modified project's memory-management instrumentation and stress-test functionality.

## 7. Results Summary

| Test | Configuration | Result |
|------|---------------|--------|
| cowtest | 200 pages, 4 children | 20 COW faults |
| memtop | After COW | 198 allocated, 27 COW faults, 0 evictions |
| stresstest | 40-frame cap, 6 workers, 15 rounds | 303 file faults, 212 evictions, 158 reloads |
| memtop | After stress | 313 file faults, 46 COW faults, 212 evictions, 158 reloads |
| Stock xv6 forktest | Basic fork baseline | fork test OK |

## 8. Limitations

Page replacement is restricted to clean, reloadable pages. Writable heap/stack pages are not swapped because disk-backed swap for writable pages was not implemented.

The project also has a known small physical-page leak observed during the broader usertests run; this should be reported honestly rather than presenting the implementation as leak-free.

## 9. Conclusion

The benchmark results demonstrate the three key behaviors relevant to this project: copy-on-write sharing and copying, demand-loaded file-backed pages, and page replacement under an artificial physical-memory cap. The stress test produced 212 evictions and 158 reloads, providing direct evidence that the replacement mechanism operates under memory pressure.
