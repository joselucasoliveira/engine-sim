#include "scheduler.hpp"

#include <algorithm>

namespace engine_sim::fleet {

void Voice::attachTimeline(es_rt_id id, TimelineTrace observer, void *context) noexcept {
    timelineId = id;
    trace = observer;
    traceContext = context;
    runtime.m_session->pull->setLatchObserver([](void *source, std::uint64_t frame, std::uint64_t tick) noexcept {
        auto &voice = *static_cast<Voice *>(source);
        if (voice.requestedSequence == voice.latchedSequence) return;
        voice.latchedSequence = voice.requestedSequence;
        if (voice.controlsPolicy == ES_RT_RAW) voice.applied = voice.requested;
        voice.latchFrame = voice.renderOrigin + (frame - voice.localOrigin);
        voice.latchTick = tick;
        if (voice.trace) voice.trace(voice.traceContext,
            {TimelinePoint::Kind::latch, voice.latchedSequence, voice.latchFrame, frame, tick, voice.timelineId});
    }, this);
}

void Voice::request(es_rt_vector vector, std::uint64_t sequence, std::uint64_t frame) noexcept {
    requested = vector;
    requestedSequence = sequence;
    requestedFrame = frame;
    runtime.setControls({sizeof(es_controls), ES_ABI_V2, vector.ignition, vector.starter,
                         vector.demand, vector.load_nm, vector.driven_rpm});
}

ProcessingStatus Voice::renderAt(std::uint64_t start, float *output, std::uint32_t count, std::uint32_t &written) {
    renderOrigin = start;
    localOrigin = runtime.telemetry().frames;
    return runtime.renderChecked(output, count, written);
}

void Scheduler::observeApply(const es_rt_event &event, const Voice *voice, es_rt_id id) const noexcept {
    if (observer) observer(traceContext, {TimelinePoint::Kind::apply, event.sequence, event.frame,
                                           voice ? voice->runtime.telemetry().frames : ES_RT_NO_FRAME,
                                           ES_RT_NO_FRAME, id});
}

std::int32_t Scheduler::apply(const es_rt_event &event) noexcept {
    if (event.kind == ES_RT_MASTER) {
        master = event.gain;
        observeApply(event, nullptr, {});
        return ES_RT_OK;
    }
    if (event.kind == ES_RT_ACTIVATE) {
        if (event.next_voice.lane >= fleet.config.lanes || event.candidate.cell >= fleet.cellCount ||
            event.candidate.epoch != fleet.config.epoch) return ES_RT_STALE;
        auto &lane = lanes[event.next_voice.lane];
        if (lane.cell != no_cell) return ES_RT_BUSY;
        auto &cell = fleet.cells[event.candidate.cell];
        if (cell.state.load(std::memory_order_acquire) != CellState::queued ||
            cell.generation.load(std::memory_order_relaxed) != event.candidate.generation || !cell.voice)
            return ES_RT_STALE;
        lane.id = event.next_voice;
        lane.cell = event.candidate.cell;
        auto &voice = *cell.voice;
        voice.attachTimeline(lane.id, observer, traceContext);
        voice.request(voice.initial, event.sequence, event.frame);
        voice.lastSequence = event.sequence;
        observeApply(event, &voice, lane.id);
        return ES_RT_OK;
    }
    if (event.voice.lane >= fleet.config.lanes) return ES_RT_STALE;
    auto &lane = lanes[event.voice.lane];
    if (lane.cell == no_cell || lane.id.generation != event.voice.generation) return ES_RT_STALE;
    auto &voice = *fleet.cells[lane.cell].voice;
    if (event.kind == ES_RT_STOP && event.duration_ms == 0) {
        voice.lastSequence = event.sequence;
        observeApply(event, &voice, lane.id);
        lane = {};
        return ES_RT_OK;
    }
    if (voice.fault != ES_RT_OK || voice.runtime.telemetry().state != ES_READY) return ES_RT_STATE;
    if (event.kind == ES_RT_GAIN) {
        voice.gain = event.gain;
    } else if (event.kind == ES_RT_VECTOR) {
        if ((voice.mode == ES_RT_FREE && event.physical.driven_rpm != 0) ||
            (voice.mode == ES_RT_DRIVEN && event.physical.starter)) return ES_RT_UNSUPPORTED;
        if (event.physical.driven_rpm > voice.drivenLimitRpm || !validVector(event.physical)) return ES_RT_INVALID;
        voice.request(event.physical, event.sequence, event.frame);
    } else {
        return ES_RT_UNSUPPORTED;
    }
    voice.lastSequence = event.sequence;
    observeApply(event, &voice, lane.id);
    return ES_RT_OK;
}

void Scheduler::snapshot(es_rt_snapshot &out) const noexcept {
    out = {};
    out.size = sizeof(out);
    out.abi = ES_RT_V1;
    out.lanes = fleet.config.lanes;
    out.epoch = fleet.config.epoch;
    out.generated = next;
    for (std::size_t index = 0; index < fleet.config.lanes; ++index) {
        const auto &lane = lanes[index];
        if (lane.cell == no_cell) {
            auto &vacant = out.lane[index];
            vacant.state = ES_RT_VACANT;
            vacant.requested_frame = ES_RT_NO_FRAME;
            vacant.latch_frame = ES_RT_NO_FRAME;
            vacant.latch_tick = ES_RT_NO_FRAME;
        } else {
            out.lane[index] = fleet.cells[lane.cell].voice->laneInfo(lane.id, ES_RT_PLAY);
        }
    }
}

void Scheduler::renderRange(ScheduledTile &output, std::uint32_t begin, std::uint32_t end) {
    if (begin == end) return;
    for (std::size_t index = 0; index < fleet.config.lanes; ++index) {
        const auto &lane = lanes[index];
        if (lane.cell == no_cell) continue;
        auto &voice = *fleet.cells[lane.cell].voice;
        if (voice.fault != ES_RT_OK || voice.runtime.telemetry().state != ES_READY) continue;
        auto *destination = output.stem[index].data() + begin;
        std::uint32_t written = 0;
        try {
            const auto status = voice.renderAt(output.first + begin, destination, end - begin, written);
            if (status != ProcessingStatus::ok)
                voice.fault = status == ProcessingStatus::render_capacity ? ES_RT_CAPACITY : ES_RT_FAULT;
        } catch (...) {
            voice.fault = ES_RT_INTERNAL;
        }
        // The initialized suffix remains silent for a failed voice.
    }
}

std::int32_t Scheduler::renderTile(ScheduledTile &output, SealObserver afterSnapshot, void *context) {
    if (next % quantum != 0 || next > UINT64_MAX - quantum) return ES_RT_CAPACITY;
    const auto end = next + quantum;
    auto &transport = fleet.commands;
    auto &queue = transport.ingress;
    const auto limit = queue.snapshot();
    if (afterSnapshot) afterSnapshot(context);
    transport.sealed.store(end, std::memory_order_release);
    output = {};
    output.first = next;
    std::uint32_t offset = 0;
    unsigned applied = 0;
    for (unsigned inspected = 0; inspected < ingress_slots; ++inspected) {
        const auto *front = queue.peek(limit);
        if (!front || front->event.frame >= end) break;
        const auto command = *front;
        queue.consume();
        if (command.event.frame < next) {
            finishCommand(fleet, command, ES_RT_LATE, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
            continue;
        }
        if (applied == 16) {
            finishCommand(fleet, command, ES_RT_FULL, ES_RT_NO_FRAME, ES_RT_NO_FRAME);
            continue;
        }
        const auto location = static_cast<std::uint32_t>(command.event.frame - next);
        renderRange(output, offset, location);
        offset = location;
        ++applied;
        const auto status = apply(command.event);
        const auto frame = status == ES_RT_OK ? command.event.frame : ES_RT_NO_FRAME;
        finishCommand(fleet, command, static_cast<std::uint32_t>(status), frame, frame);
    }
    renderRange(output, offset, quantum);
    next = end;
    return ES_RT_OK;
}

}
