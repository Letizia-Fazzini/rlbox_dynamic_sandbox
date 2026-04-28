# RLBox Process Sandbox

A process-isolation backend for [RLBox](https://github.com/PLSysSec/rlbox). Each sandboxed function call runs in a fresh OS process, so we can compare per-call process isolation against the stock wasm2c backend.

## Layout

| Path | What it is |
| --- | --- |
| `include/rlbox_process_sandbox.hpp` | Host-side sandbox class (implements the RLBox `T_Sbx` contract). |
| `src/rlbox_process_sandbox_shim.cpp` | `LD_PRELOAD`-ed shim the child process runs; hosts the RPC server, libffi dispatch, and trampoline pool. |
| `c_src/process_sandbox_wrapper.c` | Minimal wrapper binary that loads the target library and blocks forever so the shim can handle RPCs. |
| `include/rlbox_process_abi.hpp` | Wire schema shared between host and shim (argument type tags). |
| `include/rlbox_process_mem.hpp`, `include/rlbox_process_tls.hpp`, `src/rlbox_process_tls.cpp` | Shared-memory allocator + thread-local sandbox pointer used by RLBox's `_no_ctx` helpers. |
| `include/rlbox_meta_sandbox.hpp` | Meta-sandbox that composes the process and wasm2c backends and dispatches each call through a policy hook (used by the `adaptive` backend). |
| `test/test_*.cpp` | Unit + integration tests for pointer ops, memory alignment, and end-to-end invoke/callback paths. |
| `test/libjpeg-testing/` | End-to-end libjpeg port. `main.cpp` runs under wasm2c, `main_process.cpp` runs under the process backend, and `main_adaptive.cpp` runs under the meta-sandbox. |
| `test/zlib-testing/` | End-to-end zlib port (currently not functional). |
| `benchmarks/` | Benchmark harness comparing native, wasm2c, process, and adaptive backends on libjpeg. |

## Building and running the tests

For information on running the tests refer to the README.md within the `/benchmarks` directory. For general build instructions:

```bash
cmake -S . -B ./build
cmake --build ./build --parallel
```

The host<->shim wire protocol is selectable at configure time:

```bash
cmake -DRLBOX_TRANSPORT=rpclib -S . -B build_rpclib   # default
cmake -DRLBOX_TRANSPORT=capnp  -S . -B build_capnp    # Cap'n Proto over UNIX socket
```

When `capnp` is selected, Cap'n Proto is fetched via FetchContent if not present on the system. Both transports pass the same test suite; the benchmarks harness drives both and reports a side-by-side comparison.

Dev build (warnings-as-errors, address sanitizer, clang-tidy if installed):

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DDEV=ON -S . -B ./build
```


