#ifndef ENGINE_SIM_TEST_REALTIME_KERNEL_H
#define ENGINE_SIM_TEST_REALTIME_KERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

// The owner supplies a context prepared outside the callback.
struct probe_context;
int probe_callback(const char *control, struct probe_context *context, float *out, const float *in);
int probe_nested_allocation(void);

#ifdef __cplusplus
}
#endif
#endif
