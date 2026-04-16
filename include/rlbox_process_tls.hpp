#pragma once

namespace rlbox {

class rlbox_process_sandbox;

namespace detail {
  // Thread-local variable to track the currently active sandbox context.
  extern thread_local rlbox_process_sandbox* thread_local_sandbox;
}

}
