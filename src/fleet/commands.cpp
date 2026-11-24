#include "owner.hpp"
#include "envelope.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace engine_sim::fleet {
namespace {

bool empty(const es_rt_id &value) noexcept {
    return value.generation == 0 && value.lane == 0 && value.reserved == 0;
}
bool empty(const es_rt_token &value) noexcept {
    return value.epoch == 0 && value.generation == 0 && value.cell == 0 && value.reserved == 0;
}
bool empty(const es_rt_vector &value) noexcept {
    return value.demand == 0 && value.load_nm == 0 && value.driven_rpm == 0 && value.ignition == 0 && value.starter == 0;
}
bool usable(const es_rt_id &value, const es_rt_fleet &fleet) noexcept {
    return value.generation != 0 && value.reserved == 0 && value.lane < fleet.config.lanes;
}
bool hasCandidate(std::uint32_t kind) noexcept { return kind == ES_RT_ACTIVATE || kind == ES_RT_REPLACE; }
bool structural(std::uint32_t kind) noexcept { return kind >= ES_RT_MUTE || kind == ES_RT_ACTIVATE; }

const LaneIdentity *admittedIdentity(const es_rt_fleet &fleet, const es_rt_id &id) noexcept {
    const auto &lane = fleet.commands.lanes[id.lane];
    if (id.generation <= lane.retiredThrough) return nullptr;
    if (lane.current.generation == id.generation) return &lane.current;
    return lane.replacement.generation == id.generation ? &lane.replacement : nullptr;
}

std::int32_t validatePayload(const es_rt_fleet &fleet, const es_rt_event &event,
                            std::uint32_t &priorCell) noexcept {
    if (event.kind < ES_RT_ACTIVATE || event.kind > ES_RT_REPLACE) return ES_RT_INVALID;
    if (event.kind == ES_RT_VECTOR) {
        if (event.duration_ms != 0 || !validVector(event.physical)) return ES_RT_INVALID;
    } else if (!empty(event.physical) || (event.duration_ms > 100 && event.duration_ms != ES_RT_DFLT_MS)) {
        return ES_RT_INVALID;
    }
    if ((event.kind == ES_RT_GAIN || event.kind == ES_RT_MASTER) ? !finiteRange(event.gain, 1) : event.gain != 0)
        return ES_RT_INVALID;
    if (event.kind == ES_RT_MASTER) {
        return empty(event.voice) && empty(event.next_voice) && empty(event.candidate) ? ES_RT_OK : ES_RT_INVALID;
    }
    if (event.kind == ES_RT_ACTIVATE) {
        if (!empty(event.voice)) return ES_RT_INVALID;
    } else {
        if (!usable(event.voice, fleet)) return ES_RT_INVALID;
        const auto *identity = admittedIdentity(fleet, event.voice);
        if (!identity) return ES_RT_STALE;
        const auto &cell = fleet.cells[identity->cell];
        const auto state = cell.state.load(std::memory_order_acquire);
        if ((state != CellState::queued && state != CellState::active) ||
            cell.generation.load(std::memory_order_relaxed) != identity->cellGeneration) return ES_RT_STALE;
        if (event.kind == ES_RT_REPLACE || event.kind == ES_RT_STOP) priorCell = identity->cell;
        if (event.kind == ES_RT_VECTOR) {
            const auto *target = cell.voice.get();
            if (!target) return ES_RT_STALE;
            if ((target->mode == ES_RT_FREE && event.physical.driven_rpm != 0) ||
                (target->mode == ES_RT_DRIVEN && event.physical.starter)) return ES_RT_UNSUPPORTED;
            if (event.physical.driven_rpm > target->drivenLimitRpm) return ES_RT_INVALID;
        }
    }
    if (!hasCandidate(event.kind)) {
        return empty(event.next_voice) && empty(event.candidate) ? ES_RT_OK : ES_RT_INVALID;
    }
    if (!usable(event.next_voice, fleet) || event.candidate.reserved ||
        (event.kind == ES_RT_REPLACE && event.voice.lane != event.next_voice.lane)) return ES_RT_INVALID;
    const auto &lane = fleet.commands.lanes[event.next_voice.lane];
    if (lane.highWater == UINT64_MAX) return ES_RT_CAPACITY;
    if (event.next_voice.generation != lane.highWater + 1) return ES_RT_STALE;
    if (event.kind == ES_RT_ACTIVATE && lane.current.generation != 0) return ES_RT_BUSY;
    if (event.kind == ES_RT_REPLACE && (fleet.commands.overlap || lane.current.generation != event.voice.generation))
        return ES_RT_BUSY;
    return ES_RT_OK;
}

void settle(es_rt_fleet &fleet, const es_rt_result &result) noexcept {
    auto &commands = fleet.commands;
    if (hasCandidate(result.kind)) {
        assert(fleet.prepared != 0);
        --fleet.prepared;
        auto &lane = commands.lanes[result.next_voice.lane];
        if (result.kind == ES_RT_ACTIVATE) {
            if (result.applied == ES_RT_NO_FRAME && lane.current.generation == result.next_voice.generation) lane.current = {};
        } else {
            assert(commands.overlap && lane.replacement.generation == result.next_voice.generation);
            if (result.reason == ES_RT_OK)
                lane.current = lane.replacement.generation > lane.retiredThrough ? lane.replacement : LaneIdentity{};
            lane.replacement = {};
            commands.overlap = false;
        }
    } else if (result.kind == ES_RT_STOP && result.reason == ES_RT_OK) {
        auto &lane = commands.lanes[result.voice.lane];
        lane.retiredThrough = std::max(lane.retiredThrough, result.voice.generation);
        if (lane.current.generation <= lane.retiredThrough) lane.current = {};
    }
}

}

void finishCommand(es_rt_fleet &fleet, const Command &command, std::uint32_t reason,
                   std::uint64_t applied, std::uint64_t ended) noexcept {
    auto &ticket = fleet.commands.tickets[command.ticket];
    assert(ticket.state.load(std::memory_order_relaxed) == TicketState::pending &&
           ticket.sequence == command.event.sequence);
    const auto &event = command.event;
    if (hasCandidate(event.kind)) {
        auto &cell = fleet.cells[event.candidate.cell];
        // A lifecycle ticket can finish after W already adopted the cell.
        if (cell.state.load(std::memory_order_relaxed) == CellState::queued) {
            if (reason == ES_RT_OK) cell.state.store(CellState::active, std::memory_order_release);
            else returnCell(cell);
        }
    }
    if (reason == ES_RT_OK && command.priorCell != no_cell) {
        auto &prior = fleet.cells[command.priorCell];
        assert(prior.state.load(std::memory_order_relaxed) == CellState::active);
        returnCell(prior);
    }
    ticket.result = {sizeof(es_rt_result), ES_RT_V1, reason, event.kind, event.sequence, event.epoch,
                     event.frame, applied, ended, event.voice, event.next_voice};
    ticket.state.store(TicketState::done, std::memory_order_release);
}

}

extern "C" std::int32_t ES_RT_CALL es_rt_submit(es_rt_fleet *fleet, const es_rt_event *input) {
    using namespace engine_sim::fleet;
    if (!fleet || !input) return ES_RT_INVALID;
    if (const auto status = prefix(*input); status != ES_RT_OK) return status;
    const auto phase = fleet->producer.phase.load(std::memory_order_relaxed);
    if (phase == FleetPhase::stopped || fleet->pcm.broken()) return ES_RT_STATE;
    const auto event = *input;
    auto &commands = fleet->commands;
    if (event.epoch != fleet->config.epoch) return ES_RT_STALE;
    const auto index = commands.ingress.writeIndex();
    if (index == UINT64_MAX || commands.lastSequence == UINT64_MAX || event.frame > UINT64_MAX - quantum)
        return ES_RT_CAPACITY;
    if (event.sequence <= commands.lastSequence || event.frame < commands.lastFrame) return ES_RT_ORDER;
    if (event.frame < commands.sealed.load(std::memory_order_acquire)) return ES_RT_LATE;
    const auto submitted = commands.submitted.load(std::memory_order_acquire);
    if (event.frame > submitted && event.frame - submitted > 2 * fleet->config.rate) return ES_RT_INVALID;
    auto prior = no_cell;
    if (const auto status = validatePayload(*fleet, event, prior); status != ES_RT_OK) return status;
    constexpr auto lastBoundary = UINT64_MAX - UINT64_MAX % quantum;
    if (fadeFrames(event, fleet->config.rate) > lastBoundary - event.frame) return ES_RT_CAPACITY;
    auto *cell = hasCandidate(event.kind) ? preparedCell(*fleet, event.candidate) : nullptr;
    if (hasCandidate(event.kind) && !cell) return ES_RT_STALE;
    if (phase == FleetPhase::running) {
        if (const auto status = producerCommand(event); status != ES_RT_OK) return status;
    }

    const auto bucket = event.frame / quantum;
    const auto inBucket = bucket == commands.bucket ? commands.inBucket : 0;
    const auto ordinaryInBucket = bucket == commands.bucket ? commands.ordinaryInBucket : 0;
    const bool ordinary = !structural(event.kind);
    const auto available = ordinary ? ingress_slots - structural_reserve : ingress_slots;
    const auto full = [&]() noexcept {
        if (commands.rejections != UINT64_MAX) ++commands.rejections;
        return ES_RT_FULL;
    };
    if (inBucket >= 16 || (ordinary && ordinaryInBucket >= 12) ||
        index - commands.ingress.acquireHead() >= available || commands.outstanding >= available)
        return full();
    std::size_t ticketIndex = 0;
    while (ticketIndex < commands.tickets.size() &&
           commands.tickets[ticketIndex].state.load(std::memory_order_relaxed) != TicketState::vacant) ++ticketIndex;
    if (ticketIndex == commands.tickets.size()) return full();

    // All checks above are read-only. The operations below cannot fail.
    if (cell) {
        auto &lane = commands.lanes[event.next_voice.lane];
        const LaneIdentity identity{event.next_voice.generation, event.candidate.generation, event.candidate.cell};
        lane.highWater = identity.generation;
        if (event.kind == ES_RT_ACTIVATE) lane.current = identity;
        else {
            lane.replacement = identity;
            commands.overlap = true;
        }
        cell->identity = event.next_voice;
        cell->state.store(CellState::queued, std::memory_order_relaxed);
    }
    auto &ticket = commands.tickets[ticketIndex];
    ticket.sequence = event.sequence;
    ticket.state.store(TicketState::pending, std::memory_order_relaxed);
    ++commands.outstanding;
    commands.lastSequence = event.sequence;
    commands.lastFrame = event.frame;
    commands.bucket = bucket;
    commands.inBucket = inBucket + 1;
    commands.ordinaryInBucket = ordinaryInBucket + ordinary;
    commands.ingress.publish(index, {event, prior, static_cast<std::uint16_t>(ticketIndex)});
    return ES_RT_OK;
}

extern "C" std::int32_t ES_RT_CALL es_rt_poll_result(es_rt_fleet *fleet, std::uint64_t sequence, es_rt_result *out) {
    using namespace engine_sim::fleet;
    if (!fleet || !out) return ES_RT_INVALID;
    if (const auto status = prefix(*out); status != ES_RT_OK) return status;
    for (auto &ticket : fleet->commands.tickets) {
        if (ticket.sequence != sequence) continue;
        const auto state = ticket.state.load(std::memory_order_acquire);
        if (state == TicketState::vacant) continue;
        if (state == TicketState::pending) return ES_RT_PENDING;
        auto result = ticket.result;
        settle(*fleet, result);
        result.size = out->size;
        result.abi = out->abi;
        *out = result;
        ticket.state.store(TicketState::vacant, std::memory_order_relaxed);
        --fleet->commands.outstanding;
        return ES_RT_OK;
    }
    return ES_RT_STALE;
}
