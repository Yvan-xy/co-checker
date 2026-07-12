#ifndef CO_CHECKER_ONLINE_C_API_H
#define CO_CHECKER_ONLINE_C_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(CO_CHECKER_ONLINE_C_API_BUILD)
#define CO_CHECKER_ONLINE_C_API __declspec(dllexport)
#else
#define CO_CHECKER_ONLINE_C_API __declspec(dllimport)
#endif
#else
#define CO_CHECKER_ONLINE_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CO_CHECKER_ONLINE_ABI_VERSION_V1 UINT32_C(1)

typedef struct co_checker_online_runtime_v1 co_checker_online_runtime_v1;

typedef uint32_t co_checker_online_status_v1;
enum {
  CO_CHECKER_ONLINE_STATUS_OK_V1               = 0,
  CO_CHECKER_ONLINE_STATUS_INVALID_ARGUMENT_V1 = 1,
  CO_CHECKER_ONLINE_STATUS_RUNTIME_ERROR_V1    = 2,
  CO_CHECKER_ONLINE_STATUS_RESOURCE_ERROR_V1   = 3,
  CO_CHECKER_ONLINE_STATUS_STOPPED_V1          = 4,
  CO_CHECKER_ONLINE_STATUS_BUFFER_TOO_SMALL_V1 = 5
};

typedef uint32_t co_checker_online_verdict_status_v1;
enum {
  CO_CHECKER_ONLINE_VERDICT_UNAVAILABLE_V1 = 0,
  CO_CHECKER_ONLINE_VERDICT_PASS_V1        = 1,
  CO_CHECKER_ONLINE_VERDICT_VIOLATION_V1   = 2
};

typedef uint32_t co_checker_online_adapter_error_v1;
enum {
  CO_CHECKER_ONLINE_ADAPTER_ERROR_NONE_V1            = 0,
  CO_CHECKER_ONLINE_ADAPTER_ERROR_MALFORMED_INPUT_V1 = 1,
  CO_CHECKER_ONLINE_ADAPTER_ERROR_RESOURCE_V1        = 2,
  CO_CHECKER_ONLINE_ADAPTER_ERROR_LIFECYCLE_V1       = 3,
  CO_CHECKER_ONLINE_ADAPTER_ERROR_EXTERNAL_V1        = 4
};

typedef uint32_t co_checker_online_mode_v1;
enum {
  CO_CHECKER_ONLINE_MODE_CAPTURE_ONLY_V1 = 1,
  CO_CHECKER_ONLINE_MODE_ORDER_ONLY_V1   = 2,
  CO_CHECKER_ONLINE_MODE_FULL_V1         = 3
};

typedef struct co_checker_online_options_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t queue_capacity_bytes;
  uint64_t reorder_capacity_bytes;
  uint64_t capture_capacity_bytes;
  uint64_t retained_state_capacity_bytes;
  uint64_t queue_timeout_ns;
  uint64_t capture_timeout_ns;
  uint64_t chunk_segment_bytes;
  uint64_t max_cached_chunk_segments;
  uint64_t cluster_epoch;
  uint32_t mode;
  uint32_t reserved0;
  uint64_t reserved[3];
} co_checker_online_options_v1;

typedef struct co_checker_online_epoch_v1 {
  uint64_t word0;
  uint64_t word1;
} co_checker_online_epoch_v1;

typedef struct co_checker_online_uid_v1 {
  uint64_t word0;
  uint64_t word1;
} co_checker_online_uid_v1;

typedef struct co_checker_online_session_v1 {
  co_checker_online_epoch_v1 runtime_epoch;
  uint32_t producer_id;
  uint32_t reserved0;
  uint64_t local_session_counter;
} co_checker_online_session_v1;

// Pointer-free producer input. All offsets are relative to the first byte of
// the buffer passed to publish. Multi-byte fields use the host ABI byte order;
// the native adapter copies every field with memcpy and accepts unaligned
// buffers. The complete buffer is borrowed only for the duration of publish.
typedef struct co_checker_online_txn_input_header_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t total_size;
  uint64_t start_ts;
  uint64_t commit_ts;
  uint32_t read_only;
  uint32_t event_count;
  uint64_t events_offset;
  uint64_t reserved[2];
} co_checker_online_txn_input_header_v1;

typedef struct co_checker_online_event_input_v1 {
  uint32_t sequence;
  uint8_t operation;
  uint8_t value_state;
  uint8_t read_source;
  uint8_t flags;
  uint32_t key_offset;
  uint32_t key_size;
  uint32_t value_offset;
  uint32_t value_size;
  uint64_t reserved;
} co_checker_online_event_input_v1;

enum {
  CO_CHECKER_ONLINE_EVENT_READ_V1    = 1,
  CO_CHECKER_ONLINE_EVENT_WRITE_V1   = 2,
  CO_CHECKER_ONLINE_VALUE_PRESENT_V1 = 1,
  CO_CHECKER_ONLINE_VALUE_ABSENT_V1  = 2,
  CO_CHECKER_ONLINE_READ_SNAPSHOT_V1 = 1,
  CO_CHECKER_ONLINE_READ_LOCAL_V1    = 2
};

enum {
  CO_CHECKER_ONLINE_VIOLATION_NONE_V1                       = 0,
  CO_CHECKER_ONLINE_VIOLATION_NO_COMPATIBLE_SNAPSHOT_V1     = 1,
  CO_CHECKER_ONLINE_VIOLATION_INCONSISTENT_REPEATED_READ_V1 = 2
};

typedef struct co_checker_online_verdict_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t status;
  uint32_t violation_kind;
  uint64_t commit_seq;
  co_checker_online_uid_v1 transaction_id;
  co_checker_online_session_v1 session_id;
  uint64_t session_seq;
  uint64_t reserved[3];
} co_checker_online_verdict_v1;

typedef struct co_checker_online_error_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t present;
  uint32_t adapter_error;
  uint32_t runtime_error;
  uint32_t source_error;
  uint32_t external_error;
  uint32_t reserved0;
  uint64_t message_size;
  uint64_t reserved[3];
} co_checker_online_error_info_v1;

CO_CHECKER_ONLINE_C_API void
co_checker_online_options_init_v1(co_checker_online_options_v1 *options);
CO_CHECKER_ONLINE_C_API void co_checker_online_txn_input_header_init_v1(
    co_checker_online_txn_input_header_v1 *header);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_runtime_create_v1(const co_checker_online_options_v1 *options,
                                    const uint32_t *required_producers,
                                    uint32_t producer_count,
                                    co_checker_online_runtime_v1 **runtime_out);

CO_CHECKER_ONLINE_C_API void
co_checker_online_runtime_destroy_v1(co_checker_online_runtime_v1 *runtime);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_runtime_epoch_v1(const co_checker_online_runtime_v1 *runtime,
                                   co_checker_online_epoch_v1 *epoch_out);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_session_create_v1(co_checker_online_runtime_v1 *runtime,
                                    uint32_t producer_id,
                                    co_checker_online_session_v1 *session_out);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_logical_transaction_create_v1(
    co_checker_online_runtime_v1 *runtime,
    const co_checker_online_session_v1 *session, uint64_t session_seq,
    co_checker_online_uid_v1 *transaction_out);

// Synchronous: success means the active-start guard has linearized in the
// commit-order gate before this function returns.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_attempt_register_v1(
    co_checker_online_runtime_v1 *runtime,
    const co_checker_online_session_v1 *session,
    const co_checker_online_uid_v1 *transaction_id, uint64_t session_seq,
    uint64_t start_ts, co_checker_online_uid_v1 *attempt_out);

// Aborts one physical attempt. The logical transaction identity remains valid
// so an automatic retry can register a fresh attempt.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_attempt_abort_v1(co_checker_online_runtime_v1 *runtime,
                                   const co_checker_online_uid_v1 *attempt_id);

// On success ownership has transferred into the native runtime and the caller
// may immediately reuse/free the flat input buffer.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_attempt_publish_v1(co_checker_online_runtime_v1 *runtime,
                                     const co_checker_online_uid_v1 *attempt_id,
                                     const void *flat_input,
                                     uint64_t flat_input_size);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_database_frontier_v1(co_checker_online_runtime_v1 *runtime,
                                       uint64_t resolved_ts);

// The delivery cutoff is captured under the same per-producer mutex used to
// assign ProducerSeq and publish, so the frontier cannot overtake a local
// batch waiting to enter the runtime.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_producer_frontier_v1(co_checker_online_runtime_v1 *runtime,
                                       uint32_t producer_id,
                                       uint64_t resolved_ts);

// Stops admission, flushes all publications, rejects active/incomplete
// evidence, closes frontiers at the highest observed token, drains, verifies
// quiescence, and joins the consumer.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_quiescent_close_v1(co_checker_online_runtime_v1 *runtime);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1 co_checker_online_fatal_v1(
    co_checker_online_runtime_v1 *runtime, uint32_t external_error,
    const char *message, uint64_t message_size);

CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_verdict_get_v1(const co_checker_online_runtime_v1 *runtime,
                                 co_checker_online_verdict_v1 *verdict_out);

// message_size_out includes the trailing NUL. Passing NULL/zero for message
// queries the required size and returns BUFFER_TOO_SMALL when an error exists.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_error_get_v1(const co_checker_online_runtime_v1 *runtime,
                               co_checker_online_error_info_v1 *error_out,
                               char *message, uint64_t message_capacity,
                               uint64_t *message_size_out);

// json_size_out includes the trailing NUL.
CO_CHECKER_ONLINE_C_API co_checker_online_status_v1
co_checker_online_metrics_json_v1(const co_checker_online_runtime_v1 *runtime,
                                  char *json, uint64_t json_capacity,
                                  uint64_t *json_size_out);

#ifdef __cplusplus
}
#endif

#endif
