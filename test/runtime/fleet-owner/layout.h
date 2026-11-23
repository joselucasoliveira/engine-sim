#ifndef ENGINE_SIM_TEST_FLEET_LAYOUT_H
#define ENGINE_SIM_TEST_FLEET_LAYOUT_H
#include <stddef.h>
#include <engine_sim/fleet.h>

#ifdef __cplusplus
#define CHECK_LAYOUT static_assert
#else
#define CHECK_LAYOUT _Static_assert
#endif

CHECK_LAYOUT(sizeof(es_rt_id) == 16, "es_rt_id size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_id, generation) == 0, "es_rt_id.generation offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_id, lane) == 8, "es_rt_id.lane offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_id, reserved) == 12, "es_rt_id.reserved offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_token) == 24, "es_rt_token size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_token, epoch) == 0, "es_rt_token.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_token, generation) == 8, "es_rt_token.generation offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_token, cell) == 16, "es_rt_token.cell offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_token, reserved) == 20, "es_rt_token.reserved offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_vector) == 32, "es_rt_vector size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_vector, demand) == 0, "es_rt_vector.demand offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_vector, load_nm) == 8, "es_rt_vector.load_nm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_vector, driven_rpm) == 16, "es_rt_vector.driven_rpm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_vector, ignition) == 24, "es_rt_vector.ignition offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_vector, starter) == 28, "es_rt_vector.starter offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_config) == 40, "es_rt_config size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, size) == 0, "es_rt_config.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, abi) == 4, "es_rt_config.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, rate) == 8, "es_rt_config.rate offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, profile) == 12, "es_rt_config.profile offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, max_frames) == 16, "es_rt_config.max_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, lanes) == 20, "es_rt_config.lanes offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, flags) == 24, "es_rt_config.flags offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, qualification_id) == 28, "es_rt_config.qualification_id offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_config, epoch) == 32, "es_rt_config.epoch offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_voice_config) == 72, "es_rt_voice_config size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, size) == 0, "es_rt_voice_config.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, abi) == 4, "es_rt_voice_config.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, mode) == 8, "es_rt_voice_config.mode offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, simulation_hz) == 12, "es_rt_voice_config.simulation_hz offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, controls_policy) == 16, "es_rt_voice_config.controls_policy offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, reserved) == 20, "es_rt_voice_config.reserved offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, seed) == 24, "es_rt_voice_config.seed offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, starter_torque_nm) == 32, "es_rt_voice_config.starter_torque_nm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_voice_config, initial) == 40, "es_rt_voice_config.initial offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_diagnostic) == 528, "es_rt_diagnostic size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_diagnostic, size) == 0, "es_rt_diagnostic.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_diagnostic, abi) == 4, "es_rt_diagnostic.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_diagnostic, stage) == 8, "es_rt_diagnostic.stage offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_diagnostic, code) == 12, "es_rt_diagnostic.code offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_diagnostic, message) == 16, "es_rt_diagnostic.message offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_event) == 136, "es_rt_event size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, size) == 0, "es_rt_event.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, abi) == 4, "es_rt_event.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, kind) == 8, "es_rt_event.kind offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, duration_ms) == 12, "es_rt_event.duration_ms offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, epoch) == 16, "es_rt_event.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, sequence) == 24, "es_rt_event.sequence offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, frame) == 32, "es_rt_event.frame offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, voice) == 40, "es_rt_event.voice offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, next_voice) == 56, "es_rt_event.next_voice offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, candidate) == 72, "es_rt_event.candidate offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, physical) == 96, "es_rt_event.physical offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_event, gain) == 128, "es_rt_event.gain offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_result) == 88, "es_rt_result size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, size) == 0, "es_rt_result.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, abi) == 4, "es_rt_result.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, reason) == 8, "es_rt_result.reason offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, kind) == 12, "es_rt_result.kind offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, sequence) == 16, "es_rt_result.sequence offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, epoch) == 24, "es_rt_result.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, requested) == 32, "es_rt_result.requested offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, applied) == 40, "es_rt_result.applied offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, ended) == 48, "es_rt_result.ended offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, voice) == 56, "es_rt_result.voice offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_result, next_voice) == 72, "es_rt_result.next_voice offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_lane_info) == 208, "es_rt_lane_info size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, voice) == 0, "es_rt_lane_info.voice offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, state) == 16, "es_rt_lane_info.state offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, fault) == 20, "es_rt_lane_info.fault offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, frames) == 24, "es_rt_lane_info.frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, ticks) == 32, "es_rt_lane_info.ticks offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, native_frames) == 40, "es_rt_lane_info.native_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, last_sequence) == 48, "es_rt_lane_info.last_sequence offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, requested_frame) == 56, "es_rt_lane_info.requested_frame offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, last_latched_sequence) == 64, "es_rt_lane_info.last_latched_sequence offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, latch_frame) == 72, "es_rt_lane_info.latch_frame offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, latch_tick) == 80, "es_rt_lane_info.latch_tick offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, requested) == 88, "es_rt_lane_info.requested offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, applied) == 120, "es_rt_lane_info.applied offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, measured_rpm) == 152, "es_rt_lane_info.measured_rpm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, cycle_rad) == 160, "es_rt_lane_info.cycle_rad offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, plate_angle_rad) == 168, "es_rt_lane_info.plate_angle_rad offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, load_reaction_nm) == 176, "es_rt_lane_info.load_reaction_nm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, drive_reaction_nm) == 184, "es_rt_lane_info.drive_reaction_nm offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, gain) == 192, "es_rt_lane_info.gain offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_lane_info, gate) == 200, "es_rt_lane_info.gate offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_snapshot) == 1704, "es_rt_snapshot size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, size) == 0, "es_rt_snapshot.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, abi) == 4, "es_rt_snapshot.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, lanes) == 8, "es_rt_snapshot.lanes offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, reserved) == 12, "es_rt_snapshot.reserved offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, epoch) == 16, "es_rt_snapshot.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, generated) == 24, "es_rt_snapshot.generated offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, dropped) == 32, "es_rt_snapshot.dropped offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_snapshot, lane) == 40, "es_rt_snapshot.lane offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_transport) == 80, "es_rt_transport size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, size) == 0, "es_rt_transport.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, abi) == 4, "es_rt_transport.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, low_frames) == 8, "es_rt_transport.low_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, high_frames) == 12, "es_rt_transport.high_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, storage_frames) == 16, "es_rt_transport.storage_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, broken) == 20, "es_rt_transport.broken offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, returned_cells) == 24, "es_rt_transport.returned_cells offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, outstanding) == 28, "es_rt_transport.outstanding offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, epoch) == 32, "es_rt_transport.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, submitted) == 40, "es_rt_transport.submitted offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, generated) == 48, "es_rt_transport.generated offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, sealed) == 56, "es_rt_transport.sealed offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, queue_rejections) == 64, "es_rt_transport.queue_rejections offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_transport, dropped_snapshots) == 72, "es_rt_transport.dropped_snapshots offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_audio) == 88, "es_rt_audio size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, size) == 0, "es_rt_audio.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, abi) == 4, "es_rt_audio.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, frames) == 8, "es_rt_audio.frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, capacity) == 12, "es_rt_audio.capacity offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, mix) == 16, "es_rt_audio.mix offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_audio, stem) == 24, "es_rt_audio.stem offset differs from the v1 contract");
CHECK_LAYOUT(sizeof(es_rt_block) == 32, "es_rt_block size differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, size) == 0, "es_rt_block.size offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, abi) == 4, "es_rt_block.abi offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, pcm_frames) == 8, "es_rt_block.pcm_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, fallback_frames) == 12, "es_rt_block.fallback_frames offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, epoch) == 16, "es_rt_block.epoch offset differs from the v1 contract");
CHECK_LAYOUT(offsetof(es_rt_block, first) == 24, "es_rt_block.first offset differs from the v1 contract");

#undef CHECK_LAYOUT
#endif
