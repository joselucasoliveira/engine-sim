#ifndef ENGINE_SIM_TEST_FLEET_PROCESS_BOUNDARY_HPP
#define ENGINE_SIM_TEST_FLEET_PROCESS_BOUNDARY_HPP

#include "engine_sim/fleet.h"

using ProcessBoundary = int (*)(es_rt_fleet *, const es_rt_audio *, es_rt_block *, bool);
extern "C" {
int fleetProcessDirect(es_rt_fleet *, const es_rt_audio *, es_rt_block *, bool);
int fleetProcessShared(es_rt_fleet *, const es_rt_audio *, es_rt_block *, bool);
}
#endif
