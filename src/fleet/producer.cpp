#include "producer.hpp"
#include "scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <future>

namespace engine_sim::fleet {
namespace {

static_assert(tileFrames == quantum);

std::int32_t produce(es_rt_fleet &fleet, const std::atomic<bool> &stopping,
                     std::promise<std::int32_t> &startup, bool &primed) {
    Scheduler scheduler(fleet);
    const auto high = PcmTransport::highFrames(fleet.config);
    const auto low = PcmTransport::lowFrames(fleet.config);
    auto &published = fleet.producer.generated;
    const auto check = [&]() noexcept {
        if (stopping.load(std::memory_order_acquire)) return ES_RT_CANCELED;
        if (fleet.pcm.broken()) return ES_RT_BROKEN;
        return ES_RT_OK;
    };
    for (;;) {
        auto submitted = fleet.commands.submitted.load(std::memory_order_acquire);
        if (const auto status = check(); status != ES_RT_OK) return status;
        auto generated = scheduler.generated();
        if (generated < submitted) return ES_RT_INTERNAL;
        if (generated - submitted <= low) {
            while (generated - submitted <= high - quantum) {
                ScheduledTile rendered;
                if (const auto status = scheduler.renderTile(rendered); status != ES_RT_OK) return status;
                if (const auto status = check(); status != ES_RT_OK) return status;
                PcmTile output;
                output.first = rendered.first;
                output.epoch = fleet.config.epoch;
                output.stem = rendered.stem;
                for (std::uint32_t frame = 0; frame < quantum; ++frame) {
                    double mixed = 0;
                    for (std::uint32_t lane = 0; lane < fleet.config.lanes; ++lane)
                        mixed += output.stem[lane][frame];
                    output.mix[frame] = static_cast<float>(std::clamp(mixed * scheduler.masterGain(), -1., 1.));
                }
                if (!fleet.pcm.tryPublish(output)) return fleet.pcm.broken() ? ES_RT_BROKEN : ES_RT_INTERNAL;
                generated = scheduler.generated();
                published.store(generated, std::memory_order_release);
                submitted = fleet.commands.submitted.load(std::memory_order_acquire);
                if (const auto status = check(); status != ES_RT_OK) return status;
                if (generated < submitted) return ES_RT_INTERNAL;
            }
        }
        if (!primed && generated == high) {
            startup.set_value(ES_RT_OK);
            primed = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
}

}

std::int32_t producerCommand(const es_rt_fleet &fleet, const es_rt_event &event) noexcept {
    if (event.kind == ES_RT_VECTOR) return ES_RT_OK;
    if (event.duration_ms != 0) return ES_RT_UNSUPPORTED;
    if (event.kind == ES_RT_STOP) return ES_RT_OK;
    if (event.kind != ES_RT_ACTIVATE) return ES_RT_UNSUPPORTED;
    const auto &voice = fleet.cells[event.candidate.cell].voice;
    if (!voice || voice->controlsPolicy != ES_RT_RAW) return ES_RT_UNSUPPORTED;
    for (std::size_t lane = 0; lane < fleet.config.lanes; ++lane) {
        if (lane != event.next_voice.lane && fleet.commands.lanes[lane].current.generation)
            return ES_RT_UNSUPPORTED;
    }
    return ES_RT_OK;
}

std::int32_t Producer::start(es_rt_fleet &fleet) {
    if (phase.load(std::memory_order_relaxed) != FleetPhase::initial) return ES_RT_STATE;
    const auto &ingress = fleet.commands.ingress;
    const auto last = ingress.snapshot();
    for (auto index = ingress.acquireHead(); index < last; ++index) {
        const auto &event = ingress.atBeforeStart(index).event;
        if (const auto status = producerCommand(fleet, event); status != ES_RT_OK) return status;
    }
    std::promise<std::int32_t> startup;
    auto ready = startup.get_future();
    phase.store(FleetPhase::starting, std::memory_order_release);
    try {
        worker = std::thread([this, &fleet, startup = std::move(startup)]() mutable {
            bool primed = false;
            std::int32_t status = ES_RT_INTERNAL;
            try {
                status = produce(fleet, stopping, startup, primed);
            } catch (...) {
                // Unexpected producer errors close startup or leave the last
                // published PCM intact for the callback to drain.
            }
            if (!primed) startup.set_value(status);
        });
        const auto result = ready.get();
        if (result == ES_RT_OK) phase.store(FleetPhase::running, std::memory_order_release);
        else quiesce();
        return result;
    } catch (...) {
        quiesce();
        throw;
    }
}

void Producer::quiesce() {
    stopping.store(true, std::memory_order_release);
    if (worker.joinable()) worker.join();
    phase.store(FleetPhase::stopped, std::memory_order_release);
}

}

extern "C" {
std::int32_t ES_RT_CALL es_rt_get_transport(es_rt_fleet *fleet, es_rt_transport *out) {
    using namespace engine_sim::fleet;
    if (!fleet || !out) return ES_RT_INVALID;
    if (const auto status = prefix(*out); status != ES_RT_OK) return status;
    std::uint32_t returned = 0;
    for (std::size_t index = 0; index < fleet->cellCount; ++index) {
        if (fleet->cells[index].state.load(std::memory_order_acquire) == CellState::returned) ++returned;
    }
    *out = {out->size, out->abi,
        PcmTransport::lowFrames(fleet->config), PcmTransport::highFrames(fleet->config),
        static_cast<std::uint32_t>(PcmTransport::slotCount(fleet->config) * tileFrames),
        fleet->pcm.broken() ? 1u : 0u, returned, static_cast<std::uint32_t>(fleet->commands.outstanding),
        fleet->config.epoch, fleet->commands.submitted.load(std::memory_order_acquire),
        fleet->producer.generated.load(std::memory_order_acquire),
        fleet->commands.sealed.load(std::memory_order_acquire), 0, 0};
    return ES_RT_OK;
}

std::int32_t ES_RT_CALL es_rt_quiesce(es_rt_fleet *fleet) {
    if (!fleet) return ES_RT_INVALID;
    try {
        fleet->producer.quiesce();
        return ES_RT_OK;
    } catch (...) {
        return ES_RT_INTERNAL;
    }
}
}
