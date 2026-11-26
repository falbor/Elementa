# Liquid accuracy and performance audit — 2026-11-12

Status: **open**. The preceding performance work did not establish physical
accuracy or interactive performance for all liquid scenes.

## Compact pressure islands ? 2026-11-14

The disconnected-domain pressure solve now remaps each island into contiguous
local arrays. Matrix and triangular-preconditioner coefficients are prepared
once per projection rather than repeatedly loaded/multiplied through full-grid
indices. Every absent/dry neighbor reads an immutable local zero sentinel.
Only the final pressure is scattered back to the world. Matrix coefficients,
MIC tau (0.97), SSOR weight (1.7), stopping tolerances and iteration limits are
unchanged. The single-domain/coarse path remains unchanged.

The parallel-vs-serial regression now includes two pools surrounded by air,
including a fractional ghost-fluid surface, in addition to sealed chambers
and density contrast. This catches incorrect compact mapping of dry neighbors;
an initial development version failed the benchmark for that reason and was
corrected before delivery. Repeated scheduling must produce identical velocities.
Native CTest passed 11/11 with the new regression. All six GPU scenarios passed
on RTX 4070 Ti (`build/compact-pressure-gpu-world.log`). Local Docker Desktop
GCC 12 checks passed: ThreadSanitizer 1/1 (`build/compact-pressure-tsan.log`)
and ASan/UBSan 5/5 (`build/compact-pressure-sanitizers.log`).

Rejected pressure experiments: SSOR 1.9 did not improve the three presets;
Zero-G actually uses MIC. Coarse correction on parallel islands reduced
iterations but increased setup/solve time, including with independent-column
selection. MIC tau 1.0 increased Zero-G pressure time to about 46 ms. All these
experiments and temporary instrumentation were removed. Parallel interface
reconstruction also had mixed results (about +1.8% speed in Lab, -1.6% in
Zero-G) and was removed. Existing accurate PLIC transport is retained.

All six scenes completed 600 CPU steps at 480?270 with zero unconverged
projections (`build/compact-pressure-long-run.log`); maximum observed RMS
divergence was 1.59367e-6 and maximum final liquid fill 1.00021. The delivered
`build/pixel_sim.exe` matches Release SHA256
`E0BBC26B06F6E73AE75F16D05D672966495C514EF99D9ACDF3E3B82F6B84DE99`.
Previous build: `build/pixel_sim.before-compact-pressure.exe`.

Final benchmark data: `build/compact-pressure-performance.json`. Measurements
are simulation plus CPU rendering at 480?270, not GUI FPS. Performance remains
below a 60-step/s target; this change is an improvement, not a claim that the
entire performance/physical-accuracy audit is closed.

## Capacity correction: coarse vacancies then fine fallback — 2026-11-13

Capacity routing now first seeks cells with at least 1e-4 spare volume, avoiding
repeated saturation of roundoff-sized vacancies. Any remaining excess is then
routed using the original 1e-12 spare-capacity threshold. Both passes retain
the original 2e-6 excess tolerance, positive shared fluxes, donor limits, and
per-cell capacity. A pass is skipped when no excess remains. This changes
the order of flux cancellation, not the accepted volume bound.

The new microscopic-capacity regression fills a sealed tank with water/oil and
opposing face speeds of 1.5e-5. All vacancies are below the first-pass threshold;
the fine pass must still remove overfill while conserving each species and
mixture ratio. Native CTest passed **11/11** (`build/spare-threshold-ctest.log`),
including rotation, settling, walls, subnormal quantities and reused workspace.
All six GPU-world scenarios passed (`build/spare-threshold-gpu-world.log`).

The first GCC sanitizer run stopped at a settling assertion, without a sanitizer
diagnostic: it counted only cells with volume fraction above 0.9 and demanded
more than 48 compact units. A flat pool of 64 units in the 23-column basin has
only 46 units in its two full layers and 18 in a 0.7826-cell surface layer, so
that criterion rejected a valid equilibrium. The test now measures missing
volume directly in the two interior layers (less than 0.1 unit total), while
retaining the existing mass, floor-collection and surface-level checks.
The original failing log is retained as `build/spare-threshold-sanitizers.log`.
The corrected settling test passed natively (core deficits 0.001794 and
0.001903 out of 46 units) and the final local Docker ASan/UBSan run passed
**5/5** (`build/spare-threshold-sanitizers-final.log`). All six presets also
passed 600 CPU steps each (`build/spare-threshold-long-run.log`), with maximum
RMS divergence 1.59367e-6 and final maximum fills no larger than 1.00016.
The Hydro GPU capture was visually inspected (`build/spare-threshold-hydro.png`)
on RTX 4070 Ti, driver 621.24; the smooth rendering is preserved.
The final rotation audit gives relative L1 0.00923681 / 0.00476257 at
64×64 / 128×128 (`build/spare-threshold-rotation.log`). The delivered root
EXE matches Release SHA256
`AE1D5BFD24AB0F8E002C2089436C6FD3C7252371271F27A9394752DAC3B2C31A`;
the previous release is `build/pixel_sim.before-capacity-priority.exe`.

Three alternating 60-step CPU measurements at 480×270, median ms/tick:

| Preset | Previous | Current |
| --- | ---: | ---: |
| Hydro | 72.3162 | 64.3518 |
| Laboratory | 79.8721 | 78.1105 |
| ZeroG | 85.9023 | 85.7047 |

Evidence: `build/spare-threshold-performance.json`; the measured binary is
preserved in `build/pixel_sim.spare-threshold-measured.exe` (SHA256
`DACBF52AB1C4F0A86D80D65313A061AD8F342C6EAA0FC7F7174A9D57C7008CD9`).
The subsequent build only adds source comments and a test. Measurements include
simulation plus CPU rendering, not GUI FPS. The gain is material in Hydro,
small in Laboratory and negligible in ZeroG; interactive performance is still
unresolved.

A preceding whole-path cancellation trial was rejected: Hydro 72.3915 →
370.57 ms/tick, Laboratory 78.3605 → 98.0158, ZeroG 85.2238 → 108.162
(`build/path-correction-trial.json`). No part of that algorithm remains.

## Rejected active-donor CFL trial — 2026-11-13

A trial recomputed the outgoing CFL bound from currently wet donors before
each substep. The core, surface-height and rotating-interface tests passed,
but one alternating comparison gave mixed results: Hydro 72.3774 → 72.1076
ms/tick, Laboratory 78.1186 → 79.3997, ZeroG 85.8185 → 83.5066
(`build/active-cfl-trial.json`). The small inconsistent benefit did not justify
the additional scans and changed temporal subdivision. The trial was removed;
the fixed conservative outgoing-CFL subdivision is retained.

## Rejected sparse gas gather trial — 2026-11-13

A trial restricted each gas species' five-point gather to its occupied bounding
rectangle expanded by the substep count. Heat and age moments were included in
the support, and opposite-corner probes avoided a full support scan for air.
Three alternating 60-step CPU runs gave median ms/tick: Hydro 72.1643 →
72.9179, Laboratory 79.4962 → 78.6157, ZeroG 85.1773 → 85.4581.
Evidence: `build/gas-support-performance.json`. The mixed, roughly 1% changes
did not justify the extra scan and scratch initialization. The implementation
and its trial-only test were removed; the delivered transport remains unchanged.

## Rotating-interface accuracy correction — 2026-11-13

A new analytic transport audit rotates an offset liquid disk for a full turn
in a prescribed divergence-free velocity field, at 64×64 and 128×128. The
initial disk uses 16×16 subcell samples per cell. The exact final shape is the
initial shape; the metric is summed absolute volume-fraction error divided by
initial volume. This tests transport, not the coupled momentum/pressure solver.

| Grid | Previous relative L1 | Corrected relative L1 | Previous centroid error (cells) | Corrected centroid error |
| --- | ---: | ---: | ---: | ---: |
| 64×64 | 0.0858521 | 0.00931177 | 0.280440 | 0.0486528 |
| 128×128 | 0.0788435 | 0.00465806 | 0.435104 | 0.00305819 |

The previous flux estimate omitted transverse motion during each swept-strip
integration. The corrected estimate includes its midpoint displacement and a
3×3 weighted normal on curved interfaces. Detectable axis-aligned fractional
strips retain the prior normal so rectangular translation keeps sharp corners.
A weighted normal alone rounded those corners and failed the existing compact
translation test; that intermediate formulation was not retained.

`ContinuumRotationAccuracy` is now a CTest gate: relative L1 below 2%/1%,
centroid error below 0.2% of grid width, bounded nonnegative fractions, relative
mass drift below 1e-5, and at least 25% error reduction when refining the grid.
Quarter- and half-turn centroid checks also verify the direction and trajectory;
a stationary disk cannot pass merely by matching the final shape.
These are regression budgets for this reference case, not a claim of general
physical accuracy. Existing mixture/heat/age conservation, opposing-flow,
wall, subnormal, translated-rectangle and settling tests pass unchanged.
Native CTest passed **11/11** (`build/transverse-ctest.log`); the six hardware
GPU world scenarios also passed (`build/transverse-gpu-world.log`).

Three alternating CPU benchmark samples per preset measured medians:
Hydro 68.7582 → 72.2298 ms/tick, Laboratory 77.3664 → 79.2169,
ZeroG 82.6859 → 85.3662 (`build/transverse-performance.json`). The change
improves this reference accuracy at a 2–5% runtime cost; it does not solve
the performance objective. The Hydro GPU capture after 60 steps was inspected
in `build/transverse-hydro.png`: the uniform, blurred liquid appearance remains.
Local Docker Desktop GCC ASan/UBSan checks passed **5/5**, including rotation
(`build/transverse-sanitizers.log`). The verified endpoint was
`npipe:////./pipe/dockerDesktopLinuxEngine`, engine `docker-desktop`.
The corrected runtime also passed all six presets for 600 CPU steps each
(`build/transverse-long-run.log`): no pressure convergence failures, maximum
RMS divergence 1.67732e-6, and final maximum liquid fills between 0 and
1.00028. This supersedes the earlier long-run evidence for the old transport.
The delivered `build/pixel_sim.exe` and Release copy match SHA256
`7DF495BDF25714FB12087771237458FF4F3FAEC3889A7C11769850C7CC74B89F`.
The prior executable is archived as `build/pixel_sim.before-transverse.exe`.

## Extended runtime audit — 2026-11-13

Added the opt-in command `continuum_tests --long-run`: six presets at 480×270,
600 steps each. The test now stops immediately with preset/tick diagnostics
if pressure fails to converge or RMS divergence reaches 1e-4. This is ten times
the duration of the normal-resolution CTest gate; it does not replace it.

The delivered sparse-scratch implementation passed all 3600 CPU steps.
Evidence: `build/continuum-long-run.log`.

| Preset | Final maximum liquid fill | Maximum RMS divergence | Maximum pressure iterations |
| --- | ---: | ---: | ---: |
| Laboratory | 1.0001 | 4.43788e-7 | 189 |
| Volcano | 1.00017 | 9.13856e-7 | 83 |
| Hydro | 1.00014 | 1.54011e-6 | 95 |
| Explosives | 0 | 5.15726e-8 | 19 |
| Electric | 1.00011 | 2.62592e-7 | 47 |
| ZeroG | 1.00049 | 2.68596e-7 | 306 |

The fill threshold during this test remains 1.02 and every final concentration
and inspected temperature/velocity is checked for validity. These results
support longer runtime stability, not agreement with a physical reference or
interactive performance. GPU long-run coverage is still only 60 steps/preset.

A trial partitioned the liquid-flux graph into independent components and
ran their capacity corrections concurrently. One alternating comparison gave
Hydro 68.3849 → 70.7113 ms/tick, Laboratory 77.2189 → 78.5206, and
ZeroG 83.0903 → 83.9828 (`build/transport-islands-trial.json`). This did not
justify retaining the added graph traversal, so the trial was fully removed.
The pressure-island optimization remains; only the transport trial was removed.

## Transport profiling and sparse scratch initialization — 2026-11-13

Profiling Hydro over 60 CPU steps identified these liquid-advection costs:
geometry/flux construction 5.412 ms/tick, adjacency and initial distance labels
2.755, capacity correction 9.319, and positive gather 1.847. This is a
subset of the full transport stage. Raw instrumentation logs are retained in
`build/transport-profile-*.log`; instrumentation was removed from source.

Updating global distance labels every touched-cell-count/4 discharges instead
of count*4 increased correction time to 25.641 ms/tick and was rejected.
Count*16 gave 8.824 ms correction but no overall improvement (69.869 versus
69.736 ms/tick); that trial was also reverted. Neither changes the delivered
solver's refresh frequency or capacity tolerance.

Scratch adjacency heads, outgoing volumes, and future-volume buffers now
initialize only current flux endpoints. The arithmetic and flux order are
unchanged. A new regression compares reused and fresh workspaces, moving
water/oil between separated regions and including stationary steps; it checks
amount, heat and age fields exactly.

Three alternating baseline/current CPU runs, median ms/tick:
Hydro 69.7413 → 69.1721; Laboratory 78.3873 → 78.1108;
ZeroG 83.9476 → 83.6044 (`build/sparse-graph-performance.json`). The measured
difference is below 1%, too small to establish a substantial speedup.
Hydro CPU snapshots after 60 ticks have identical SHA256:
`794A688A8E944B41C75DB3BA933E1C5D29CAC29DD3ED56149DBC82E7D78C1CA3`.
The main remaining transport target is capacity correction, not buffer clears.
Native CTest passed 10/10 in 39.30 seconds, including workspace reuse
(`build/sparse-graph-ctest.log`). The verified Release was copied to
`build/pixel_sim.exe`, SHA256
`16132F4EDDB4D0B26623568D83C59EAA4EC417E72273F4FFE534797D0D6BC8B1`;
the preceding GPU build is archived as `build/pixel_sim.before-sparse-graph.exe`.

## GPU momentum integration — 2026-11-13

The GUI now uses OpenGL 3.3 float textures for RK2 velocity advection and
12 viscosity iterations. Pressure and conservative quantity transport remain
on the CPU. Startup compares a wall/variable-viscosity probe with the CPU;
runtime failure leaves input fields untouched and invokes the CPU fallback.
The core regression checks fallback equality, backend detachment, and input
validation before invoking the backend. Captures now initialize the backend
before simulation and invalidate the renderer after the requested steps.

Hardware: RTX 4070 Ti, driver 621.14. The six full-world GPU scenarios each
ran 60 steps at 480×270: all pressure solves converged, maximum liquid fill
was 1.00004, maximum RMS divergence 1.31681e-6. Evidence:
`build/gpu-world-validation.log`. The Hydro capture after 60 GPU steps was
visually inspected in `build/gpu-momentum-hydro.png`; smooth liquid rendering
and solid boundaries remain present.

Native CTest passed **10/10** in 38.97 seconds, including the new fallback
regression (`build/gpu-momentum-ctest.log`). Standalone hardware validation
passed all ten cases at 64×48 and 480×270; maximum velocity difference was
1.39028e-5. At 480×270 its isolated median was 6.4984 ms CPU versus
1.8309 ms GPU, including upload and synchronous readback
(`build/gpu-momentum-validation.log`). These checks do not constitute a GPU
memory sanitizer or a full long-running physical accuracy audit.

Three alternating CPU/GPU runs of the same executable, 60 steps at 480×270:

| Preset | CPU median ms/tick | GPU median ms/tick |
| --- | ---: | ---: |
| Hydro | 74.3229 | 71.2249 |
| Laboratory | 78.6282 | 75.2109 |
| ZeroG | 83.7259 | 82.4376 |

Evidence: `build/gpu-momentum-performance.json`, executable SHA256
`3D15687D8F2D59315119760DBFB63559E0441217D33EE37CFB53E04E2AD20585`.
Measurements include simulation and CPU rendering, not GUI FPS. The isolated
momentum gain does not translate to a comparable whole-frame gain: pressure
and transport still dominate. **Interactive performance remains unresolved.**

## Current pressure optimization: independent liquid regions

Disconnected pressure domains now solve independently on the CPU thread pool.
Each domain has its own residual norm and stopping criterion; completed domains
are no longer visited while another converges. The largest domains are queued
first. This path is selected only for grids with at least 4096 unknowns and
at least two domains of 1024 unknowns or more. A dominant tank with small
satellite droplets keeps the existing coarse/global solver.

The new path does not reduce accuracy: each component meets the original
relative tolerance separately. Zero-coefficient neighbor slots are explicitly
not read, avoiding cross-component accesses to dummy indices during concurrent
solves. The operator, pressure boundary, and amount transport remain unchanged.

Verification:

- Native CTest **10/10**, 41.69 seconds. Normal-resolution tests now require
  convergence at every tick and post-projection RMS divergence below 1e-4;
  measured maxima across all six presets stayed below 1.322e-6.
- Three two-chamber comparisons cover uniform density, density contrast 9,
  and layered variable density inside each chamber. Maximum velocity
  differences versus the global solve were 5.83e-6, 3.11e-5, and 1.78e-5.
  Repeated parallel solves return identical face velocities.
- Local Docker ASan/UBSan **4/4**, 11.74 seconds;
  `build/pressure-islands-sanitizers.log`.
- Local Docker GCC ThreadSanitizer **1/1**, 17.95 seconds, covering the
  continuum suite including parallel pressure/transport;
  `build/pressure-islands-tsan.log`. No race reports. PIE was disabled and
  IPO was off; no privileged container or host security changes were needed.

The initial unconditional parallel selection slowed Hydro and was rejected.
The final selection is based on component sizes, not preset names.
Another rejected experiment retained independent coarse columns using pivoted
Cholesky: it reduced iterations but increased total solve time. The original
coarse factorization and high-density-contrast fallback remain in use.
Final repeated comparison: `build/pressure-islands-final-performance.json`.

Final medians, three alternating runs of 60 ticks at 480x270:

| Preset | Previous ms/tick | Current ms/tick |
| --- | ---: | ---: |
| hydro | 69.5270 | 69.7316 |
| lab | 94.6331 | 77.9091 |
| zerog | 104.0400 | 83.8169 |

Root and Release EXEs now match SHA256
`B35B4BE5EEEAE820E722C2D06BF72319086AA498A429BF1E5120A10380E76752`.
The previous EBF009D1 build is archived as
`build/pixel_sim.before-pressure-islands.exe` and recorded in the comparison
JSON. The delivered root EXE passes the 32x32 CLI smoke run. The simulation
is still CPU-bound overall; this is progress, not completion of the goal.

## Current transport optimization

The liquid CFL now uses maximum outgoing donor rate, not the maximum of
incoming and outgoing rates. Donor sweeps stay below 0.9 cell volume per
substep; converging streams are still bounded by the conservative receiver
capacity correction. Existing opposing-stream, mixture, wall, high-CFL,
serial/parallel, and conservation tests cover this change. Grid resolution,
pressure tolerance, and the receiver capacity tolerance are unchanged.

All liquid face, adjacency, and cell scratch buffers now persist across
ticks and retain their capacity. Every used value is initialized by the
same per-substep passes; reset and mixture transport tests also pass.

Temporary instrumentation (removed from final source) measured Hydro's mean
substeps falling from 6.60 to 5.82. Baseline correction cost was 11.98 ms;
the outgoing-only version measured 10.76 ms. An attempted highest-height
discharge policy instead took 31.48 ms and was reverted. FIFO remains.
Raw measurements are in `build/liquid-profile-raw.log`,
`build/liquid-profile-outgoing.log`, and `build/liquid-profile-reused.log`.

Release CTest: **10/10**, 41.47 seconds. Mirrored basin residual height
differences: **0.0167634**, **0.00904982** cells, volume drift below 0.000024.
Final repeated executable comparison: `build/transport-workspace-performance.json`.

Median timings (three alternating repetitions, 480x270, 60 ticks):

| Preset | Previous ms/tick | Current ms/tick | Reduction |
| --- | ---: | ---: | ---: |
| hydro | 73.3936 | 69.5491 | 5.2% |
| lab | 104.932 | 94.3922 | 10.0% |
| zerog | 109.188 | 103.783 | 5.0% |

The final native build hash is
`EBF009D1730CEBBCBA204BD07D3266C3C5770A707EBA56D3457C593661CF576C`.
GPU lab capture at 60 ticks was inspected (`build/transport-workspace-lab.png`):
water/oil retain a horizontal interface; fluid rendering remains smooth.
These timings still leave substantial CPU work; the overall goal is not complete.

Docker Desktop was started using `docker desktop start --detach`; context and
daemon were verified as `desktop-linux`, local named pipe, `docker-desktop`.
The local `simulazioni-debug:local` image uses Ninja/GCC 12.2. A fresh readonly
source mount built with ASan+UBSan, PIE disabled, leak checks enabled.
**4/4 sanitizer tests passed** in 11.07 seconds: transport, surface accuracy,
CLI continuum smoke, and default continuum. Log:
`build/transport-workspace-sanitizers.log`. This resolves the earlier local
daemon blocker and covers the new interface/pressure and scratch storage code.

Root and Release EXEs now both have the EBF009D1 hash above. Previous root
EXE (84EFA522) is archived as `build/pixel_sim.before-workspace.exe` and
recorded in the repeated benchmark JSON. Historical delivery hashes below
refer to earlier checkpoints.

## Current follow-up: subcell pressure boundary

Delivered build SHA256: `84EFA52267FEB7A62F4395A8C523A94731EFAB8C07193E25AA960067363C7D2A`.
Root and Release EXEs match. Previous delivered build is preserved at
`build/pixel_sim.before-subcell.exe` (the baseline hash in the repeated report).
Median of three alternating 60-tick runs at 480x270:

| Preset | Previous ms/tick | Current ms/tick |
| --- | ---: | ---: |
| hydro | 72.8837 | 75.2563 |
| lab | 104.464 | 106.370 |
| zerog | 129.918 | 110.280 |

The comparison passes its 10% regression budget, but this still does not
meet an interactive frame-time budget. Performance work remains open.
GPU canvas capture `build/subcell-hydro.png` at 120 ticks was inspected on
RTX 4070 Ti, OpenGL 3.3: smooth liquid volumes remain contained by the basin.
The root EXE also passes a 32x32 CLI smoke run after replacement.

ASan/UBSan rerun is unavailable: Docker context `desktop-linux` points to
`npipe:////./pipe/dockerDesktopLinuxEngine`, but that local daemon pipe is
absent. No production or replacement Docker environment was used. Earlier
sanitizer results do not cover this pressure/interface change.

`ContinuumPressure::project` now accepts a signed-distance interface. Negative
sample centers are pressure unknowns; wet/dry coefficients include the
interpolated distance to the atmospheric boundary. The same coefficient is
used in the matrix and velocity update. Interface distances are bounded below
by 0.01 cell to avoid singular conditioning at a pressure sample. Open domain
boundaries are placed at the boundary face.

`reconstructSurface` derives the near-interface distance from volume fractions
and PLIC planes, including exact full/empty grid-aligned boundaries. The world
now uses this reconstruction for liquid pressure; no quantity is removed by
the pressure-domain classification. Air forcing has not been disabled.

New isolated checks pass for hydrostatic pressure at six fill levels (0, 0.1,
0.4, 0.6, 0.9, 1), plus mirrored fractional head differences without air forces.
The full integrated Release suite passes **10/10** (47.58 seconds). A subsequent
open-boundary correction also passes the core continuum suite. Integrated
basin height differences are now **0.0144286** and **0.0693866** cells.

This supersedes the pressure implementation described in the historical
sections below. Full performance and sanitizer verification remain incomplete;
the first sequential comparison is `build/subcell-performance.json`.
The benchmark script now defaults to three repetitions with alternating run
order and reports medians plus every timing sample. The repeated comparison
is `build/subcell-performance-repeated.json`.

## Follow-up: geometric transport and isolated pressure defect

The transport now reconstructs a linear interface (PLIC) with exact rectangle
intersection areas. Limited one-sided normals preserve flat faces at corners;
centered normals are retained where both limited slopes vanish. Reconstruction
is cached once per partial cell; full cells require no interface calculation.
Tests invert the area function and partition the cell into disjoint strips
for axis-aligned, diagonal, and reflected normals at 101 fill fractions.

The original mirrored basin regression now passes: residual height differences
are **0.0269431** and **0.0751748** cells, with volume drift below 0.000014.
It is now registered as `ContinuumSurfaceAccuracy` in CTest. The translated
48-volume rectangle retains all 48 compact units and its exact expected
centroid (16.5). The historical failure below records the pre-PLIC result.

An isolation experiment removed only the two air-to-liquid force terms.
The basin then retained a **0.800001** cell height difference after 600 ticks:
pressure alone does not respond to subcell head differences. The original
air coupling was restored after the experiment. This problem remains open
even though the integrated mirrored-world test now passes. A reconstructed
pressure boundary and consistent surface-velocity extension are still needed;
see the [Bridson fluid simulation reference material](https://www.cs.ubc.ca/~rbridson/fluidsimulation/).

Initial PLIC benchmarks before caching interface reconstruction were slower
(hydro 82.43 ms/tick, zerog 167.66 ms/tick), especially in pressure after the
changed flow trajectory. Do not interpret the accuracy improvement as a
performance fix. The PLIC build remains under evaluation; `build/pixel_sim.exe`
still contains the preceding delivered optimization, not this experimental build.

After caching reconstruction, the full Release suite passes **10/10** in
48.04 seconds. A subsequent sequential benchmark measured hydro 98.13 and
zerog 146.64 ms/tick; pressure alone cost 19.26 and 74.35 ms/tick. Timings
vary between runs and do not establish a speed improvement. The performance
gate remains open. Delivered root EXE SHA256 starts `CDA17A35F086`; the
experimental Release EXE starts `5E4DCA189A01`.

## Reproduced accuracy defect

Run `build/win-clang-release/continuum_tests.exe --accuracy-audit`.
The diagnostic constructs two horizontally mirrored, closed water basins.
Their surface row contains 0.9 cell volumes on one side and 0.1 on the other;
the three rows below are full. After 600 simulation ticks, the mean surface
height difference should be below 0.1 cell in both orientations.

Measured differences: **0.099841** and **0.268352** cells. Volume drift is
only -0.0000181552 and -0.0000174067 cell volumes, respectively. The second
orientation fails. This diagnostic is intentionally separate from the
existing CTest suite while the defect is being investigated; a green CTest
run does not mean this accuracy issue is resolved.

Pressure currently treats every cell above its volume threshold as wet and
places atmospheric pressure at dry cells. It does not reconstruct a subcell
surface for the pressure boundary. The capacity correction also chooses
paths in traversal order. These are investigation targets, not proven sole
causes of the mirrored-world discrepancy. Coupling with the air solver must
also be isolated before attributing the result to one subsystem.

## Additional evidence

All six presets at 480x270 over 60 ticks converged on every pressure solve.
Maximum post-projection RMS divergence ranged from 2.23e-7 to 1.69e-6;
maximum iterations ranged from 22 to 305. Thus pressure iteration failure
does not explain the observed accuracy defect in these runs.

The liquid gather now evaluates and commits only endpoints of proposed
fluxes instead of copying three complete grids for every active species and
substep. It retains the same conservative fluxes and positive double-precision
gather arithmetic. Gas diffusion skips absent species, and liquid separation
returns early when fewer than two liquid species exist. No concentration
cutoff, simulation resolution reduction, or relaxed pressure tolerance was
introduced.

Existing Release CTest: **9/9 passed**, including the six full-resolution
presets. The mirrored surface diagnostic above remains a known failure.
Sequential benchmark evidence is in `build/debug-audit-performance.json`;
the reference executable is `build/debug-audit-baseline.exe`, copied before
this audit's optimizations. These are simulation plus CPU-rendering timings,
not measured GUI frame rates. GPU physics is still unimplemented.

Final sequential 60-tick comparison (milliseconds per tick): hydro
74.3000 → 70.7751; lab 111.611 → 102.616; zerog 134.299 → 130.253.
This is a modest improvement, not an interactive-performance solution.
The Release executable has been copied to `build/pixel_sim.exe`; both copies
were verified identical by SHA256.
