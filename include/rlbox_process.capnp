@0xb43d44ce5b7f3e92;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("rlbox::wire");

# Wire schema for the host<->shim RPC channel when RLBOX_TRANSPORT=capnp.
#
# Mirrors the rpclib bind names and argument shapes 1:1 so the host class and
# shim dispatch logic stay structurally identical.  Each request is one
# Cap'n Proto message frame on a Unix abstract socket; the shim sends back
# one Response message per request.  Callbacks use a separate socket flowing
# the other direction (CallbackRequest -> CallbackResponse).

struct Request {
  union {
    # name to look up via dlsym(RTLD_DEFAULT, name); returns address.
    lookupSymbol @0 :Text;

    # bytes to allocate in the shared mspace; returns absolute address.
    allocate @1 :UInt64;

    # absolute address to free; returns 0 (acknowledged).
    release @2 :UInt64;

    # function call dispatched via libffi inside a per-call child.
    invoke @3 :Invoke;

    # host-side callback key; returns the trampoline's absolute address.
    registerCallback @4 :UInt64;

    # host-side callback key; returns 0 (acknowledged).
    unregisterCallback @5 :UInt64;
  }

  struct Invoke {
    funcAddr @0 :UInt64;
    retTag @1 :Int32;
    argTags @2 :List(Int32);
    argValues @3 :List(Int64);
  }
}

struct Response {
  result @0 :Int64;
}

# Child -> host: the shim's trampoline invokes a callback registered by the
# host.  Args are the int64-widened slots the trampoline received (currently
# fixed at 4 by the trampoline ABI).
struct CallbackRequest {
  key @0 :UInt64;
  args @1 :List(Int64);
}

struct CallbackResponse {
  result @0 :Int64;
}
