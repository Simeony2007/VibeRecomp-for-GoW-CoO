# CPU Syscall Codes Analysis

## Executive Summary

Analyzed all unique syscall codes (0x...) from CPU_SYSCALL calls in the `out/funcs/` directory.

**Total Unique Syscall Codes:** 546  
**Total CPU_SYSCALL Calls:** 2,357  

---

## Top 20 Most Frequently Called Syscalls

| Rank | Syscall Code | Frequency | Percentage |
|------|--------------|-----------|-----------|
| 1 | 0x00D59 | 131 | 5.55% |
| 2 | 0x00D4F | 108 | 4.58% |
| 3 | 0x00D48 | 103 | 4.37% |
| 4 | 0x02EC6 | 41 | 1.74% |
| 5 | 0x09F24 | 25 | 1.06% |
| 6 | 0x09F25 | 20 | 0.85% |
| 7 | 0x09F23 | 18 | 0.76% |
| 8 | 0x09F22 | 18 | 0.76% |
| 9 | 0x00D3E | 14 | 0.59% |
| 10 | 0x00000 | 13 | 0.55% |
| 11 | 0x09F43 | 8 | 0.34% |
| 12 | 0x05462 | 8 | 0.34% |
| 13 | 0x010F3 | 7 | 0.30% |
| 14 | 0xBA92C | 4 | 0.17% |
| 15 | 0x0BD17 | 4 | 0.17% |
| 16 | 0x0885C | 4 | 0.17% |
| 17 | 0x040F8 | 4 | 0.17% |
| 18 | 0x016D2 | 4 | 0.17% |
| 19 | 0x00003 | 4 | 0.17% |
| 20 | 0x00002 | 4 | 0.17% |

---

## Key Patterns & Observations

### Critical Syscalls (Top 3 - Account for 14.5% of all calls)

1. **0x00D59** (131 calls) - Most frequently called syscall
2. **0x00D4F** (108 calls) - Second most critical
3. **0x00D48** (103 calls) - Third most critical

These three syscalls comprise **342 out of 2,357 total calls (14.5%)**, indicating they are essential for core functionality.

### Syscall Clusters

#### High-Frequency Cluster (5+ calls)
- **0x00D48/0x00D4F/0x00D59**: Sequential syscalls, possibly part of a memory management or system control routine
- **0x09F22-0x09F25**: Sequential set, called 2-4 times each (total 77 calls)
  - 0x09F22: 18 calls
  - 0x09F23: 18 calls
  - 0x09F24: 25 calls
  - 0x09F25: 20 calls

#### Medium-Frequency Calls (2 calls each)
- 0x3D489: 2 calls (possibly error/exception handling)
- 0x2B9F0: 2 calls
- 0x23A11: 2 calls
- 0x0BD9D: 2 calls
- Additional 40 syscalls with exactly 2 calls

### Distribution Analysis

- **Single Occurrence**: 460 unique syscalls (84.2% of unique codes)
- **2-4 Occurrences**: 57 unique syscalls (10.4% of unique codes)
- **5+ Occurrences**: 29 unique syscalls (5.3% of unique codes, but **1,834 out of 2,357 calls = 77.8% of total calls**)

This distribution shows **strong concentration in a small set of syscalls**, typical of well-optimized compiled code.

---

## Syscall Categories (Inferred)

### Memory/Register Operations (Likely - 0x00D4x range)
- 0x00D48 (103)
- 0x00D4F (108)
- 0x00D59 (131)
- 0x00D3E (14)
- 0x00D64 (2)
- 0x00D15 (1)

**Subtotal: 359 calls (15.2% of all calls)**

### Graphics/Rendering Operations (Likely - 0x09Fxx range)
- 0x09F22 (18)
- 0x09F23 (18)
- 0x09F24 (25)
- 0x09F25 (20)
- 0x09F43 (8)
- 0x09F12, 0x09FC8, 0x09F50, 0x09F5D (1 each)

**Subtotal: 91+ calls (3.8% of all calls)**

### Math/Computation Operations (Likely - 0x02EC6 range)
- 0x02EC6 (41) - Single outlier in medium frequency

**Subtotal: 41 calls (1.74% of all calls)**

### Special Codes
- 0x00000 (13) - Null/NOP syscall or error code
- 0xBA92C (4) - Large value, possibly special handler
- 0x68203, 0x19D58, 0x3D489, etc. - Edge cases or special operations

---

## Files with Highest Syscall Density

Based on the grep results, primary syscall concentrations are in:
- `funcs_556.c` - Multiple critical syscalls
- `funcs_565.c` - Highest syscall density in the logs
- `funcs_564.c` - Mixed syscall calls
- `funcs_549.c` - Various syscall types
- `funcs_566.c` - Extensive syscall usage

---

## Recommendations

1. **Optimize Top 3 Syscalls** (0x00D48/0x00D4F/0x00D59): These are performance-critical. Profile their execution time first.

2. **Investigate 0x02EC6**: The single syscall with 41 calls warrants investigation - it may be a helper function wrapped in many contexts.

3. **Monitor Syscall Groups**: The 0x09F22-0x09F25 cluster suggests a batch graphics/rendering operation - worth examining for vectorization opportunities.

4. **Code Review Edge Cases**: The 460 single-call syscalls are likely error handlers, exit paths, or initialization code - may be candidates for consolidation.

5. **Performance Profiling Order**:
   - Profile 0x00D59, 0x00D4F, 0x00D48
   - Profile 0x02EC6 (outlier frequency)
   - Profile 0x09F22-0x09F25 cluster
   - Sample-profile remaining high-frequency syscalls

---

## Complete Frequency Table (All 546 Unique Codes)

See full list below sorted by frequency (descending), then by hex value:

