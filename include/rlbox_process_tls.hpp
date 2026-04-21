#pragma once

namespace rlbox {

class rlbox_process_sandbox;

namespace detail {
  // Thread-local pair published by the process backend's callback-
  // dispatch loop (capnp: start_callback_loop; rpclib: the server
  // handler lambda) before it invokes the user-registered
  // interceptor, and cleared when the dispatcher returns.  Read by
  // `rlbox_process_sandbox::impl_get_executed_callback_sandbox_and_key`
  // and, composed up a level, by `rlbox_meta_sandbox`'s own version
  // of that hook — both ultimately satisfy the rlbox
  // `sandbox_callback_interceptor` contract of "tell me the sandbox
  // and the user's key while the callback is mid-flight."  Declared
  // here so the meta can read them without the `friend` dance.
  extern thread_local rlbox_process_sandbox* thread_local_sandbox;
  extern thread_local void* thread_local_callback_key;
}

}
