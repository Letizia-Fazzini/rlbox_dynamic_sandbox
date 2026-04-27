#pragma once

namespace rlbox {

class rlbox_process_sandbox;

namespace detail {
  // Set by the callback-dispatch loop before invoking the interceptor and
  // cleared on return. Lets impl_get_executed_callback_sandbox_and_key
  // recover (sandbox, user-key) mid-callback. Declared here so the meta
  // sandbox can read them without `friend`.
  extern thread_local rlbox_process_sandbox* thread_local_sandbox;
  extern thread_local void* thread_local_callback_key;
}

}
