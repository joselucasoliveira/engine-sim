#ifndef ENGINE_SIM_PRIVATE_FLEET_INGRESS_HPP
#define ENGINE_SIM_PRIVATE_FLEET_INGRESS_HPP

#include "engine_sim/fleet.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace engine_sim::fleet {

constexpr std::size_t ingress_slots = 256;
constexpr std::size_t structural_reserve = 8;
constexpr std::uint64_t quantum = 64;
constexpr std::uint32_t no_cell = UINT32_MAX;

struct Command {
    es_rt_event event{};
    std::uint32_t priorCell = no_cell;
    std::uint16_t ticket = 0;
};
static_assert(std::is_trivially_copyable_v<Command>);

enum class TicketState : std::uint32_t { vacant, pending, done };
struct Ticket {
    std::atomic<TicketState> state{TicketState::vacant};
    // Sequence is issued by C and immutable through the ownership handoff.
    std::uint64_t sequence = 0;
    es_rt_result result{};
};

class Ingress {
public:
    std::uint64_t writeIndex() const noexcept { return tail.load(std::memory_order_relaxed); }
    std::uint64_t acquireHead() const noexcept { return head.load(std::memory_order_acquire); }
    std::uint64_t snapshot() const noexcept { return tail.load(std::memory_order_acquire); }

    // C owns the write index and reserves all credits before calling publish.
    void publish(std::uint64_t index, const Command &command) noexcept {
        commands[index % ingress_slots] = command;
        tail.store(index + 1, std::memory_order_release);
    }

    // W borrows the current slot until consume. Its snapshot never expands.
    const Command *peek(std::uint64_t limit) const noexcept {
        const auto index = head.load(std::memory_order_relaxed);
        return index < limit ? &commands[index % ingress_slots] : nullptr;
    }
    void consume() noexcept {
        head.store(head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    bool lockFree() const noexcept { return head.is_lock_free() && tail.is_lock_free(); }

private:
    friend struct IngressTestAccess;
    alignas(64) std::atomic<std::uint64_t> head{0};
    alignas(64) std::atomic<std::uint64_t> tail{0};
    std::array<Command, ingress_slots> commands{};
};

struct LaneIdentity {
    std::uint64_t generation = 0;
    std::uint64_t cellGeneration = 0;
    std::uint32_t cell = no_cell;
};
struct LaneCredits {
    std::uint64_t highWater = 0;
    std::uint64_t retiredThrough = 0;
    LaneIdentity current, replacement;
};

struct CommandTransport {
    Ingress ingress;
    std::array<Ticket, ingress_slots> tickets{};
    std::array<LaneCredits, 8> lanes{};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> sealed{0};
    std::uint64_t lastSequence = 0, lastFrame = 0, bucket = 0, rejections = 0;
    std::size_t outstanding = 0;
    unsigned inBucket = 0, ordinaryInBucket = 0;
    bool overlap = false;

    bool lockFree() const noexcept {
        if (!ingress.lockFree() || !submitted.is_lock_free() || !sealed.is_lock_free()) return false;
        for (const auto &ticket : tickets) if (!ticket.state.is_lock_free()) return false;
        return true;
    }
};

// Called only by W after taking the command. A returned cell stays owned and
// occupied until C performs off-thread collection.
void finishCommand(es_rt_fleet &fleet, const Command &command, std::uint32_t reason,
                   std::uint64_t applied, std::uint64_t ended) noexcept;

}
#endif
