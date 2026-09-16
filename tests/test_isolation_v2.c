/* Regression coverage for transitive batch and public metadata isolation. */
#define MAGIC2_IMPLEMENTATION
#include "../magic2.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pair_base(
    void *output, const void *input, size_t count,
    void *call_user, void *candidate_user) {
    (void)input;
    (void)count;
    (void)call_user;
    (void)candidate_user;
    *(int *)output = 1;
    return 0;
}

static int buffer_base(
    magic2_buffer_desc *buffers, size_t buffer_count, size_t count,
    void *call_user, void *candidate_user) {
    (void)count;
    (void)call_user;
    (void)candidate_user;
    if (buffer_count < 2u) return -1;
    *(int *)buffers[1].data = *(const int *)buffers[0].data + 1;
    return 0;
}

static magic2_adaptive_context *make_pair_context(void) {
    magic2_candidate candidate = magic2_candidate_initializer();
    magic2_adaptive_config config = magic2_adaptive_config_initializer();
    magic2_adaptive_context *context = NULL;
    candidate.fn = pair_base;
    candidate.stable_id.low = 1u;
    candidate.stable_id.high = 2u;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.output_bytes = sizeof(void *);
    config.maximum_input_bytes = 0u;
    config.maximum_overlap_span = sizeof(void *);
    assert(magic2_create(&config, &context) == MAGIC2_OK);
    return context;
}

static void test_batch_child_envelope_alias(void) {
    magic2_dispatch_request child = magic2_dispatch_request_initializer();
    magic2_dispatch_request batch = magic2_dispatch_request_initializer();
    const char *message = NULL;
    void *requests[1];
    int result;

    child.operation = MAGIC2_OP_ERROR_STRING;
    child.args.error_string.error_code = MAGIC2_EINVAL;
    child.args.error_string.out_string = &message;
    requests[0] = &child;
    batch.operation = MAGIC2_OP_BATCH;
    batch.args.batch.requests = requests;
    batch.args.batch.request_count = 1u;
    batch.args.batch.results = (int *)(void *)&child.args.error_string.out_string;
    batch.args.batch.processed_count = NULL;
    result = magic2(&batch);
    assert(result == MAGIC2_EOVERLAP);
    assert(child.args.error_string.out_string == &message);
}

static void test_batch_child_payload_alias(void) {
    magic2_adaptive_context *context = make_pair_context();
    magic2_call call = magic2_call_initializer();
    magic2_dispatch_request child1 = magic2_dispatch_request_initializer();
    magic2_dispatch_request child2 = magic2_dispatch_request_initializer();
    magic2_dispatch_request batch = magic2_dispatch_request_initializer();
    const char *message = NULL;
    void *requests[2];
    void *second_before;
    int result;

    call.output = &requests[1];
    call.output_bytes = sizeof(requests[1]);
    call.count = 0u;
    child1.operation = MAGIC2_OP_CONTEXT_RUN;
    child1.args.adaptive_run.context = context;
    child1.args.adaptive_run.call = &call;
    child2.operation = MAGIC2_OP_ERROR_STRING;
    child2.args.error_string.error_code = MAGIC2_EINVAL;
    child2.args.error_string.out_string = &message;
    requests[0] = &child1;
    requests[1] = &child2;
    second_before = requests[1];
    batch.operation = MAGIC2_OP_BATCH;
    batch.args.batch.requests = requests;
    batch.args.batch.request_count = 2u;
    result = magic2(&batch);
    assert(result == MAGIC2_EOVERLAP);
    assert(requests[1] == second_before);
    assert(magic2_destroy(&context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
}

static magic2_adaptive_buffer_context *make_buffer_context(void) {
    magic2_adaptive_buffer_candidate candidate =
        magic2_adaptive_buffer_candidate_initializer();
    magic2_adaptive_buffer_config config =
        magic2_adaptive_buffer_config_initializer();
    magic2_adaptive_buffer_context *context = NULL;
    candidate.fn = buffer_base;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.maximum_buffer_count = 2u;
    config.session_count = 1u;
    config.maximum_total_buffer_bytes = 64u;
    config.async_operation_capacity = 0u;
    assert(magic2_buffer_context_create(&config, &context) == MAGIC2_OK);
    return context;
}

static void test_buffer_status_call_alias(int through_dispatch) {
    magic2_adaptive_buffer_context *context = make_buffer_context();
    magic2_buffer_desc buffers[2];
    magic2_adaptive_buffer_call call =
        magic2_adaptive_buffer_call_initializer();
    magic2_dispatch_request request = magic2_dispatch_request_initializer();
    int input = 41;
    int output = 0;
    magic2_adaptive_buffer_call before;
    int result;

    buffers[0] = magic2_buffer_initializer();
    buffers[0].data = &input;
    buffers[0].bytes = sizeof(input);
    buffers[0].alignment = 1u;
    buffers[0].access = MAGIC2_BUFFER_READ;
    buffers[1] = magic2_buffer_initializer();
    buffers[1].data = &output;
    buffers[1].bytes = sizeof(output);
    buffers[1].alignment = 1u;
    buffers[1].access = MAGIC2_BUFFER_WRITE;
    call.buffers = buffers;
    call.buffer_count = 2u;
    call.count = 1u;
    before = call;
    if (through_dispatch) {
        request.operation = MAGIC2_OP_BUFFER_CONTEXT_RUN;
        request.args.buffer_run.context = context;
        request.args.buffer_run.call = &call;
        request.args.buffer_run.status = (magic2_run_status *)(void *)&call;
        result = magic2(&request);
    } else {
        result = magic2_buffer_context_run(
            context, &call, (magic2_run_status *)(void *)&call);
    }
    assert(result == MAGIC2_EOVERLAP);
    assert(memcmp(&call, &before, sizeof(call)) == 0);
    assert(output == 0);
    assert(magic2_buffer_context_destroy(
               &context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
}

static void test_profile_export_alias(void) {
    magic2_candidate candidate = magic2_candidate_initializer();
    magic2_adaptive_config config = magic2_adaptive_config_initializer();
    magic2_adaptive_context *context = NULL;
    magic2_dispatch_request request = magic2_dispatch_request_initializer();
    unsigned char *buffer;
    unsigned char *before;
    size_t required = 0u;
    size_t index;
    int result;

    candidate.fn = pair_base;
    candidate.stable_id.low = 3u;
    candidate.stable_id.high = 4u;
    candidate.capabilities = MAGIC2_CAP_BASELINE_REQUIRED;
    config.candidates = &candidate;
    config.candidate_count = 1u;
    config.output_bytes = sizeof(int);
    config.maximum_input_bytes = sizeof(int);
    config.maximum_overlap_span = sizeof(int) * 2u;
    assert(magic2_create(&config, &context) == MAGIC2_OK);
    assert(magic2_profile_export(context, NULL, 0u, &required) == MAGIC2_OK);
    assert(required != 0u);
    buffer = (unsigned char *)malloc(required);
    before = (unsigned char *)malloc(required);
    assert(buffer != NULL && before != NULL);
    memset(buffer, 0xa5, required);
    memcpy(before, buffer, required);
    result = magic2_profile_export(
        context, buffer, required, (size_t *)(void *)buffer);
    assert(result == MAGIC2_EOVERLAP);
    for (index = 0u; index < required; ++index)
        assert(buffer[index] == before[index]);
    memset(buffer, 0xa5, required);
    request.operation = MAGIC2_OP_PROFILE_EXPORT;
    request.args.adaptive_profile_export.context = context;
    request.args.adaptive_profile_export.buffer = buffer;
    request.args.adaptive_profile_export.capacity = required;
    request.args.adaptive_profile_export.actual_size =
        (size_t *)(void *)buffer;
    result = magic2(&request);
    assert(result == MAGIC2_EOVERLAP);
    for (index = 0u; index < required; ++index)
        assert(buffer[index] == before[index]);
    free(before);
    free(buffer);
    assert(magic2_destroy(&context, MAGIC2_DESTROY_DRAIN) == MAGIC2_OK);
}

int main(void) {
    test_batch_child_envelope_alias();
    test_batch_child_payload_alias();
    test_buffer_status_call_alias(0);
    test_buffer_status_call_alias(1);
    test_profile_export_alias();
    puts("magic2 transitive isolation regressions passed");
    return 0;
}
