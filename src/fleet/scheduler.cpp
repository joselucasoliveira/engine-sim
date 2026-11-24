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

Scheduler::~Scheduler() {
    for (auto &lane : lanes) cancel(lane, ES_RT_CANCELED, ES_RT_NO_FRAME);
}

void Scheduler::cancel(ScheduledLane &lane, std::uint32_t reason, std::uint64_t frame) noexcept {
    if (!lane.transition) return;
    const auto pending = *lane.transition;
    lane.transition.reset();
    finishCommand(fleet, pending, reason, pending.event.frame, frame);
}

namespace {
double residual(const Voice &voice, std::uint64_t frame) noexcept {
    if (voice.fault == ES_RT_OK || frame < voice.failureFrame) return 0.;
    return faultSample(voice.lastSample, frame - voice.failureFrame);
}

std::uint32_t generate(Voice &voice, std::uint64_t first, float *output, std::uint32_t count) {
    if (voice.fault != ES_RT_OK) return 0;
    std::uint32_t written = 0;
    try {
        const auto status = voice.renderAt(first, output, count, written);
        if (status != ProcessingStatus::ok)
            voice.fault = status == ProcessingStatus::render_capacity ? ES_RT_CAPACITY : ES_RT_FAULT;
    } catch (...) {
        voice.fault = ES_RT_INTERNAL;
    }
    if (written) voice.lastSample = output[written - 1];
    if (voice.fault != ES_RT_OK) voice.failureFrame = first + written;
    return written;
}
}

void Scheduler::complete(ScheduledLane &lane, std::uint64_t frame) noexcept {
    if (!lane.transition || lane.endpoint() > frame) return;
    auto &voice = *fleet.cells[lane.cell].voice;
    voice.gate = lane.gate.at(frame);
    voice.gain = lane.gain.at(frame);
    const auto pending = *lane.transition;
    lane.transition.reset();
    std::uint32_t reason = ES_RT_OK;
    if (lane.state == ES_RT_OVERLAP) {
        const auto &prior = *fleet.cells[lane.oldCell].voice;
        if (voice.fault != ES_RT_OK || prior.fault != ES_RT_OK) reason = ES_RT_FAULT;
        lane.oldCell = no_cell;
        lane.state = voice.fault != ES_RT_OK ? ES_RT_FAILED : lane.muted ? ES_RT_MUTED : ES_RT_PLAY;
    } else if (lane.state == ES_RT_STOPPING) {
        lane = {};
    } else if (lane.state == ES_RT_PAUSING) {
        lane.state = ES_RT_PAUSED;
    } else {
        lane.state = lane.muted ? ES_RT_MUTED : ES_RT_PLAY;
    }
    finishCommand(fleet, pending, reason, pending.event.frame, frame);
}

std::int32_t Scheduler::transition(ScheduledLane &lane, const Command &command,
                                   std::uint32_t state, double target, std::uint32_t frames) noexcept {
    const auto frame = command.event.frame;
    cancel(lane, ES_RT_INTERRUPTED, frame);
    lane.gate.retarget(frame, frames, target);
    lane.state = state;
    lane.transition = command;
    auto &voice = *fleet.cells[lane.cell].voice;
    voice.gate = lane.gate.at(frame);
    complete(lane, frame);
    return ES_RT_PENDING;
}

void Scheduler::adopt(ScheduledLane &lane, Cell &cell, const es_rt_event &event) noexcept {
    lane.id = event.next_voice;
    lane.cell = event.candidate.cell;
    auto &voice = *cell.voice;
    voice.attachTimeline(lane.id, observer, traceContext);
    voice.request(voice.initial, event.sequence, event.frame);
    voice.lastSequence = event.sequence;
    voice.gain = lane.gain.at(event.frame);
    voice.gate = lane.gate.at(event.frame);
    observeApply(event, &voice, lane.id);
    cell.state.store(CellState::active, std::memory_order_release);
}

std::int32_t Scheduler::replace(ScheduledLane &lane, const Command &command) noexcept {
    const auto &event = command.event;
    if (lane.state != ES_RT_PLAY && lane.state != ES_RT_MUTED && lane.state != ES_RT_FAILED) return ES_RT_BUSY;
    auto &old = fleet.cells[lane.cell];
    if (lane.state == ES_RT_FAILED && event.frame - old.voice->failureFrame < faultFrames) return ES_RT_BUSY;
    if (event.candidate.epoch != fleet.config.epoch || event.candidate.cell >= fleet.cellCount ||
        command.priorCell != lane.cell || event.next_voice.lane != event.voice.lane) return ES_RT_STALE;
    auto &cell = fleet.cells[event.candidate.cell];
    if (cell.state.load(std::memory_order_acquire) != CellState::queued ||
        cell.generation.load(std::memory_order_relaxed) != event.candidate.generation || !cell.voice)
        return ES_RT_STALE;
    lane.oldCell = lane.cell;
    lane.blend = Envelope(0);
    lane.blend.retarget(event.frame, fadeFrames(event, fleet.config.rate), 1.);
    lane.state = ES_RT_OVERLAP;
    lane.transition = command;
    old.voice->lastSequence = event.sequence;
    old.state.store(CellState::overlap_old, std::memory_order_release);
    adopt(lane, cell, event);
    complete(lane, event.frame);
    return ES_RT_PENDING;
}

std::int32_t Scheduler::apply(const Command &command) noexcept {
    const auto &event = command.event;
    if (event.kind == ES_RT_MASTER) {
        if (master.target() != event.gain) master.retarget(event.frame, fadeFrames(event, fleet.config.rate), event.gain);
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
        adopt(lane, cell, event);
        return transition(lane, command, ES_RT_ATTACK, 1., fadeFrames(event, fleet.config.rate));
    }
    if (event.voice.lane >= fleet.config.lanes) return ES_RT_STALE;
    auto &lane = lanes[event.voice.lane];
    if (lane.cell == no_cell || lane.id.generation != event.voice.generation) return ES_RT_STALE;
    if (lane.state == ES_RT_STOPPING ||
        (lane.state == ES_RT_OVERLAP && event.kind != ES_RT_GAIN && event.kind != ES_RT_VECTOR)) return ES_RT_BUSY;
    auto &voice = *fleet.cells[lane.cell].voice;
    if (event.kind == ES_RT_REPLACE) return replace(lane, command);
    if (event.kind != ES_RT_STOP && voice.fault != ES_RT_OK) return ES_RT_STATE;
    const auto duration = fadeFrames(event, fleet.config.rate);

    const auto applied = [&]() noexcept {
        voice.lastSequence = event.sequence;
        observeApply(event, &voice, lane.id);
        return ES_RT_OK;
    };
    switch (event.kind) {
    case ES_RT_VECTOR:
        if ((voice.mode == ES_RT_FREE && event.physical.driven_rpm != 0) ||
            (voice.mode == ES_RT_DRIVEN && event.physical.starter)) return ES_RT_UNSUPPORTED;
        if (event.physical.driven_rpm > voice.drivenLimitRpm || !validVector(event.physical)) return ES_RT_INVALID;
        voice.request(event.physical, event.sequence, event.frame);
        return applied();
    case ES_RT_GAIN:
        if (lane.gain.target() != event.gain)
            lane.gain.retarget(event.frame, fadeFrames(event, fleet.config.rate), event.gain);
        return applied();
    case ES_RT_MUTE:
    case ES_RT_UNMUTE: {
        const bool mute = event.kind == ES_RT_MUTE;
        if (lane.state == ES_RT_PAUSING) return ES_RT_STATE;
        applied();
        if (lane.muted == mute) return ES_RT_OK;
        lane.muted = mute;
        if (lane.state == ES_RT_PAUSED) return ES_RT_OK;
        return transition(lane, command, mute ? ES_RT_MUTING : ES_RT_ATTACK, mute ? 0. : 1., duration);
    }
    case ES_RT_PAUSE:
        applied();
        if (lane.state == ES_RT_PAUSED || lane.state == ES_RT_PAUSING) return ES_RT_OK;
        return transition(lane, command, ES_RT_PAUSING, 0., duration);
    case ES_RT_RESUME:
        applied();
        if (lane.state != ES_RT_PAUSED && lane.state != ES_RT_PAUSING) return ES_RT_OK;
        return transition(lane, command, ES_RT_ATTACK, lane.muted ? 0. : 1., duration);
    case ES_RT_STOP: {
        applied();
        lane.forcedStop = voice.fault != ES_RT_OK;
        lane.stopSample = residual(voice, event.frame);
        const bool silent = lane.gain.at(event.frame) == 0 || (lane.forcedStop && lane.stopSample == 0);
        return transition(lane, command, ES_RT_STOPPING, 0., silent ? 0 : duration);
    }
    default:
        return ES_RT_UNSUPPORTED;
    }
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
            out.lane[index] = fleet.cells[lane.cell].voice->laneInfo(lane.id, lane.state);
        }
    }
}

void Scheduler::renderSegment(ScheduledTile &output, std::uint32_t begin, std::uint32_t end) {
    for (auto i = begin; i < end; ++i) output.master[i] = master.at(output.first + i);
    const auto first = output.first + begin;
    const auto count = end - begin;
    for (std::size_t index = 0; index < fleet.config.lanes; ++index) {
        auto &lane = lanes[index];
        if (lane.cell == no_cell) continue;
        auto &voice = *fleet.cells[lane.cell].voice;
        const auto last = first + count - 1;
        voice.gain = lane.gain.at(last);
        if (lane.state == ES_RT_PAUSED) {
            voice.gate = 0.;
            continue;
        }

        const bool faultedBefore = voice.fault != ES_RT_OK;
        const auto written = generate(voice, first, source.data(), count);
        const bool ordinaryFault = !faultedBefore && voice.fault != ES_RT_OK && lane.state != ES_RT_OVERLAP;
        const auto heldGate = ordinaryFault ? lane.gate.at(voice.failureFrame) : 0.;
        Voice *old = lane.oldCell == no_cell ? nullptr : fleet.cells[lane.oldCell].voice.get();
        const auto oldWritten = old ? generate(*old, first, previous.data(), count) : 0;
        if (old) {
            old->gain = voice.gain;
            old->gate = lane.gate.at(last);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto frame = first + i;
            auto sample = i < written ? static_cast<double>(source[i]) : residual(voice, frame);
            if (old) {
                const auto before = i < oldWritten ? static_cast<double>(previous[i]) : residual(*old, frame);
                sample = crossfade(before, sample, lane.blend.at(frame));
            } else if (lane.forcedStop) {
                sample = lane.stopSample;
            }
            const auto gate = ordinaryFault && frame >= voice.failureFrame ? heldGate : lane.gate.at(frame);
            output.stem[index][begin + i] = scaleStem(sample, lane.gain.at(frame), gate);
        }
        if (ordinaryFault) {
            cancel(lane, ES_RT_FAULT, voice.failureFrame);
            lane.gate.retarget(voice.failureFrame, 0, heldGate);
            lane.state = ES_RT_FAILED;
        }
        voice.gate = lane.gate.at(last);
    }
}

void Scheduler::renderRange(ScheduledTile &output, std::uint32_t begin, std::uint32_t end) {
    while (begin < end) {
        auto boundary = output.first + end;
        for (std::size_t i = 0; i < fleet.config.lanes; ++i) {
            const auto &lane = lanes[i];
            if (lane.transition) boundary = std::min(boundary, lane.endpoint());
        }
        const auto fragment = static_cast<std::uint32_t>(boundary - output.first);
        renderSegment(output, begin, fragment);
        for (std::size_t i = 0; i < fleet.config.lanes; ++i) complete(lanes[i], boundary);
        begin = fragment;
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
        const auto status = apply(command);
        if (status != ES_RT_PENDING) {
            const auto frame = status == ES_RT_OK ? command.event.frame : ES_RT_NO_FRAME;
            finishCommand(fleet, command, static_cast<std::uint32_t>(status), frame, frame);
        }
    }
    renderRange(output, offset, quantum);
    next = end;
    return ES_RT_OK;
}

}
