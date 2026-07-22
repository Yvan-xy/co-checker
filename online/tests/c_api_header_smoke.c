#include "co_checker/online/c_api.h"

#include <stddef.h>

_Static_assert(sizeof(co_checker_online_options_v1) == 112,
               "options V1 layout changed");
_Static_assert(offsetof(co_checker_online_options_v1, mode) == 80,
               "options mode offset changed");
_Static_assert(offsetof(co_checker_online_options_v1, ordering_protocol) == 84,
               "options ordering protocol offset changed");
_Static_assert(sizeof(co_checker_online_session_v1) == 32,
               "session V1 layout changed");
_Static_assert(sizeof(co_checker_online_txn_input_header_v1) == 64,
               "flat header V1 layout changed");
_Static_assert(sizeof(co_checker_online_event_input_v1) == 32,
               "flat event V1 layout changed");

int co_checker_online_c_header_smoke(void) {
  co_checker_online_options_v1 options;
  co_checker_online_options_init_v1(&options);
  return options.struct_size == sizeof(options) &&
                 options.abi_version == CO_CHECKER_ONLINE_ABI_VERSION_V1
             ? 0
             : 1;
}
