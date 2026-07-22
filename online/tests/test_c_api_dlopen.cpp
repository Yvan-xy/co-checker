#include "co_checker/online/c_api.h"

#include <array>
#include <cstdint>
#include <dlfcn.h>
#include <iostream>
#include <string_view>

int main(const int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "expected shared-library path\n";
    return 1;
  }
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  constexpr std::array<std::string_view, 19> symbols{
      "co_checker_online_options_init_v1",
      "co_checker_online_txn_input_header_init_v1",
      "co_checker_online_runtime_create_v1",
      "co_checker_online_runtime_destroy_v1",
      "co_checker_online_runtime_epoch_v1",
      "co_checker_online_session_create_v1",
      "co_checker_online_logical_transaction_create_v1",
      "co_checker_online_attempt_register_v1",
      "co_checker_online_attempt_abort_v1",
      "co_checker_online_attempt_prepare_v1",
      "co_checker_online_attempt_finalize_v1",
      "co_checker_online_attempt_publish_v1",
      "co_checker_online_database_frontier_v1",
      "co_checker_online_producer_frontier_v1",
      "co_checker_online_quiescent_close_v1",
      "co_checker_online_fatal_v1",
      "co_checker_online_verdict_get_v1",
      "co_checker_online_error_get_v1",
      "co_checker_online_metrics_json_v1",
  };
  for (const auto symbol : symbols) {
    if (dlsym(library, symbol.data()) == nullptr) {
      std::cerr << "missing C ABI symbol: " << symbol << '\n';
      (void)dlclose(library);
      return 1;
    }
  }

  using OptionsInit   = void (*)(co_checker_online_options_v1 *);
  using RuntimeCreate = co_checker_online_status_v1 (*)(
      const co_checker_online_options_v1 *, const std::uint32_t *,
      std::uint32_t, co_checker_online_runtime_v1 **);
  using RuntimeClose =
      co_checker_online_status_v1 (*)(co_checker_online_runtime_v1 *);
  using RuntimeDestroy    = void (*)(co_checker_online_runtime_v1 *);
  const auto options_init = reinterpret_cast<OptionsInit>(
      dlsym(library, "co_checker_online_options_init_v1"));
  const auto runtime_create = reinterpret_cast<RuntimeCreate>(
      dlsym(library, "co_checker_online_runtime_create_v1"));
  const auto runtime_close = reinterpret_cast<RuntimeClose>(
      dlsym(library, "co_checker_online_quiescent_close_v1"));
  const auto runtime_destroy = reinterpret_cast<RuntimeDestroy>(
      dlsym(library, "co_checker_online_runtime_destroy_v1"));
  co_checker_online_options_v1 options{};
  options_init(&options);
  options.mode                     = CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1;
  constexpr std::uint32_t producer = 1;
  co_checker_online_runtime_v1 *runtime = nullptr;
  if (runtime_create(&options, &producer, 1, &runtime) !=
          CO_CHECKER_ONLINE_STATUS_OK_V1 ||
      runtime == nullptr) {
    std::cerr << "dlsym runtime creation failed\n";
    (void)dlclose(library);
    return 1;
  }
  if (runtime_close(runtime) != CO_CHECKER_ONLINE_STATUS_OK_V1) {
    std::cerr << "dlsym runtime close failed\n";
    runtime_destroy(runtime);
    (void)dlclose(library);
    return 1;
  }
  runtime_destroy(runtime);
  if (dlclose(library) != 0) {
    std::cerr << dlerror() << '\n';
    return 1;
  }
  return 0;
}
