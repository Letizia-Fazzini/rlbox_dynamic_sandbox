#include "rlbox_process_tls.hpp"

namespace rlbox {
namespace detail {
  thread_local rlbox_process_sandbox* thread_local_sandbox = nullptr;
}
}
