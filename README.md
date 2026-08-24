# Kineto

Kineto is the profiling library used by PyTorch Profiler. It provides low-overhead timeline tracing, performance diagnostics, integration with system profilers, and trace output for visualization and analysis tools.

This repository is a MUSA/MUPTI-enabled Kineto fork aligned with the PyTorch 2.13 profiler architecture. For Libkineto's upstream interfaces, also see [libkineto/README.md](libkineto/README.md) and the [PyTorch Profiler documentation](https://pytorch.org/docs/stable/profiler.html).

## MUSA upgrade summary

The historical PyTorch 2.9 port was based on an older Kineto tree and replaced CUDA/CUPTI names and implementations across the source tree. That approach was useful for bringing up MUPTI, but it coupled generic profiling logic to one device backend and is not suitable for maintaining the newer Kineto architecture.

The current upgrade keeps the PyTorch 2.13 device-independent profiler structure and ports only the device-specific layer:

- `GenericActivityProfiler` owns the common CPU/GPU trace lifecycle, buffering, correlation, filtering, metadata, and trace finalization.
- `MuptiActivityProfiler` derives from `GenericActivityProfiler` and implements the MUSA/MUPTI hooks for enabling activities, processing GPU records, synchronization, and correlation IDs.
- `ActivityProfilerController` remains a thin coordinator. At build time it selects the MUPTI backend and creates the shared synchronous and asynchronous handlers.
- MUPTI initialization is deferred until tracing starts, after a possible worker fork, and its availability is cached.
- The MUPTI activity path covers runtime/driver callbacks, kernels, memcpy/memset, synchronization, event records, external correlation, buffer flushing, and trace conversion.
- MUPTI 5.1 differences are handled semantically rather than by blind renaming: unsupported timestamp callbacks and activity attributes are avoided, timestamps are converted to the trace clock, and activity record versions match the target SDK.
- Repeated profiler sessions keep the backend lifecycle usable; expensive or unsafe global finalization is not placed on the normal per-trace path.

In short, the upgrade changes the implementation from a MUSA-specific copy of the old profiler into a narrow MUPTI backend plugged into Kineto's current generic profiler core.

## Current architecture

The main execution path is:

```text
Python torch.profiler
  -> PyTorch kineto shim / profiler proxy
  -> ActivityProfilerController
  -> SyncActivityProfilerHandler or AsyncActivityProfilerHandler
  -> GenericActivityProfiler
  -> MuptiActivityProfiler / MuptiActivityApi
  -> MUPTI buffers and activity records
  -> Kineto activities
  -> Chrome/Perfetto trace
```

The division of responsibility is intentional:

- Changes to scheduling, CPU trace ingestion, common buffering, generic filtering, or output normally belong in `GenericActivityProfiler` or the handlers.
- Changes to MUPTI APIs, record layouts, MUSA synchronization, device probing, or GPU activity conversion belong in `MuptiActivityProfiler`, `MuptiActivityApi`, or the MUPTI activity wrappers.
- Backend construction and request routing should remain lightweight in `ActivityProfilerController`.

Do not reapply the old whole-tree CUDA-to-MUSA rename to this architecture. It can overwrite generic upstream changes and unnecessarily duplicate backend-independent logic.

## Activity type compatibility

`ActivityType` values are serialized into traces and consumed by other tools. Existing values and names are therefore compatibility interfaces, not free enum slots.

The current layout preserves the legacy CUDA ABI entries:

| Value | Activity type | Serialized name |
| ---: | --- | --- |
| 7 | `CUDA_RUNTIME` | `cuda_runtime` |
| 8 | `CUDA_DRIVER` | `cuda_driver` |
| 15 | `CUDA_SYNC` | `cuda_sync` |
| 16 | `CUDA_EVENT` | `cuda_event` |
| 19 | `CUDA_PROFILER_RANGE` | `cuda_profiler_range` |

MUSA uses the following entries:

| Value | Activity type | Serialized name |
| ---: | --- | --- |
| 24 | `PRIVATEUSE1_RUNTIME` | `musa_runtime` |
| 25 | `PRIVATEUSE1_DRIVER` | `musa_driver` |
| 29 | `MUSA_PROFILER_RANGE` | `musa_profiler_range` |
| 30 | `MUSA_SYNC` | `musa_sync` |
| 31 | `MUSA_EVENT` | `musa_event` |

`ENUM_COUNT` is currently `32`. When adding an activity, append a new value before `ENUM_COUNT`; do not renumber or repurpose existing serialized slots. Keep the enum and string table in [`libkineto/include/ActivityType.h`](libkineto/include/ActivityType.h) synchronized.

## Build guide

### Build Libkineto with MUSA/MUPTI

The backend is selected explicitly. `cpu` is the default, so a MUSA build must pass `KINETO_BACKEND=musa`.

```bash
export MUSA_HOME=/usr/local/musa

cmake -S libkineto -B build_musa \
  -DKINETO_BACKEND=musa \
  -DKINETO_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_musa -j"$(nproc)"
ctest --test-dir build_musa --output-on-failure
```

Configuration fails rather than silently falling back if `mupti.h`, `libmupti`, or `libmusart` cannot be found below `MUSA_HOME`. Use `-DKINETO_BACKEND=cpu` for an intentional CPU-only build.

`build_musa/` is generated build output. Do not commit or distribute it as source; consumers should rebuild it for their own compiler, SDK, and runtime environment.

### Integrate with PyTorch

When building PyTorch/torch_musa, use this repository as the Kineto source selected by the parent build instead of patching an already-built library. The integration must ensure that:

1. the MUSA PrivateUse1 backend is registered before profiler activities are requested;
2. Kineto is configured with `KINETO_BACKEND=musa` and links the MUPTI and MUSART libraries from the target MUSA SDK;
3. the PyTorch profiler shim maps the exposed MUSA/PrivateUse1 activity to Kineto's PrivateUse1 runtime and driver activities;
4. PyTorch and this Kineto tree are rebuilt together after changing profiler ABI or shim code.

The exact source override and rebuild command belongs to the parent PyTorch/torch_musa project because it controls the Kineto submodule and build cache.

## Usage guide

Load `torch_musa` before constructing the profiler so that the PrivateUse1 backend is registered and named `musa`.

```python
import torch
import torch_musa

activities = [torch.profiler.ProfilerActivity.CPU]

# The public spelling depends on the matching PyTorch/torch_musa integration.
if hasattr(torch.profiler.ProfilerActivity, "MUSA"):
    activities.append(torch.profiler.ProfilerActivity.MUSA)
elif hasattr(torch.profiler.ProfilerActivity, "PrivateUse1"):
    activities.append(torch.profiler.ProfilerActivity.PrivateUse1)
else:
    raise RuntimeError("This PyTorch build does not expose MUSA profiler activity")

x = torch.randn(1024, 1024, device="musa")
with torch.profiler.profile(
    activities=activities,
    record_shapes=True,
) as prof:
    y = x @ x
    torch.musa.synchronize()

prof.export_chrome_trace("musa_trace.json")
```

Open `musa_trace.json` in Perfetto or another Chrome trace viewer. A healthy GPU trace should contain host runtime/driver calls and device kernel or memory activities, with correlation IDs linking the corresponding CPU and GPU events.

## Validation checklist

Validate progressively so that backend failures are distinguishable from generic profiler failures:

1. **CPU-only:** collect CPU operator and Python events without requesting MUSA.
2. **Backend bring-up:** profile one MUSA kernel and confirm that MUPTI initializes rather than falling back to CPU-only behavior.
3. **Activity coverage:** confirm MUSA runtime/driver calls, kernels, HtoD/DtoH copies, memset, sync, and event records as applicable to the workload.
4. **Time and correlation:** check that timestamps are in the capture window, durations are non-negative, and runtime calls correlate with device activities.
5. **Repeated sessions:** run several profiler contexts in one process and check that later traces still receive GPU records.
6. **Concurrency:** exercise multithreaded CPU activity ingestion and synchronous/asynchronous profiler handlers.
7. **Integration:** run representative torch_musa profiler tests, including graph workloads and long traces; inspect dropped-record reporting and flush behavior.

A trace file being generated is not sufficient by itself: verify its event categories, timestamps, device/stream placement, and correlation links.

## Legacy porting tool

[`tools/kineto_mupti_port/`](tools/kineto_mupti_port/) contains an in-place CUDA/CUPTI-to-MUSA/MUPTI migration tool for reproducing or updating the historical port from a clean compatible Kineto tree.

Preview a migration with:

```bash
python3 tools/kineto_mupti_port/port_cupti_to_mupti.py \
  --src /path/to/kineto \
  --dry-run
```

The non-dry-run mode is destructive: it rewrites content and renames files in place. Use it only on a clean branch or recoverable copy and read the [tool documentation](tools/kineto_mupti_port/README.md) first.

This tool is not the normal maintenance workflow for the current 2.13 architecture. New upstream updates should be merged with the generic profiler structure intact, followed by focused MUPTI backend changes and the validation sequence above.

## Source map

Recommended reading order:

1. [`libkineto/include/ActivityType.h`](libkineto/include/ActivityType.h) — serialized activity ABI and names.
2. [`libkineto/src/ActivityProfilerController.cpp`](libkineto/src/ActivityProfilerController.cpp) — backend construction and request routing.
3. [`libkineto/src/SyncActivityProfilerHandler.cpp`](libkineto/src/SyncActivityProfilerHandler.cpp) and [`libkineto/src/AsyncActivityProfilerHandler.cpp`](libkineto/src/AsyncActivityProfilerHandler.cpp) — synchronous and asynchronous scheduling.
4. [`libkineto/src/GenericActivityProfiler.h`](libkineto/src/GenericActivityProfiler.h) and [`libkineto/src/GenericActivityProfiler.cpp`](libkineto/src/GenericActivityProfiler.cpp) — common profiler state and lifecycle.
5. [`libkineto/src/MuptiActivityProfiler.h`](libkineto/src/MuptiActivityProfiler.h) and [`libkineto/src/MuptiActivityProfiler.cpp`](libkineto/src/MuptiActivityProfiler.cpp) — MUSA-specific GPU hooks and record processing.
6. [`libkineto/src/MuptiActivityApi.h`](libkineto/src/MuptiActivityApi.h), [`libkineto/src/MuptiActivityApi.cpp`](libkineto/src/MuptiActivityApi.cpp), and [`libkineto/src/MuptiActivity.h`](libkineto/src/MuptiActivity.h) — MUPTI API wrapper, buffers, and activity conversion.
7. [`tools/kineto_mupti_port/README.md`](tools/kineto_mupti_port/README.md) — legacy migration rules and MUPTI 5.1 compatibility notes.

## Releases and contributing

Kineto follows the PyTorch release schedule, which is roughly quarterly.

Bug fixes are welcome. For new features, open an issue and discuss the design before sending a pull request because the profiler architecture continues to evolve. MUSA-specific changes should remain localized to the backend unless they are genuinely device-independent.

## License

Kineto has a BSD-style license, as found in the [LICENSE](LICENSE) file.
