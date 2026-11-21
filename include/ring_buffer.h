#ifndef ATG_ENGINE_SIM_RING_BUFFER_H
#define ATG_ENGINE_SIM_RING_BUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

// Bounded FIFO. A rejected write leaves every unread sample intact.
// Bulk destinations must have room for the requested number of elements.
template <typename T_Data>
class RingBuffer {
public:
    RingBuffer() = default;
    ~RingBuffer() { destroy(); }
    RingBuffer(const RingBuffer &) = delete;
    RingBuffer &operator=(const RingBuffer &) = delete;

    void initialize(size_t capacity) {
        if (capacity == 0) throw std::invalid_argument("Ring buffer capacity must be positive");
        T_Data *storage = new T_Data[capacity]{};
        destroy();
        m_buffer = storage;
        m_capacity = capacity;
    }

    void clear() { m_start = m_writeIndex = m_count = 0; }

    void destroy() {
        delete[] m_buffer;
        m_buffer = nullptr;
        m_capacity = 0;
        clear();
    }

    bool write(T_Data data) {
        if (m_count == m_capacity) return false;
        m_buffer[m_writeIndex] = data;
        m_writeIndex = (m_writeIndex + 1) % m_capacity;
        ++m_count;
        return true;
    }

    void overwrite(T_Data data, size_t offset) {
        if (offset >= m_count) throw std::out_of_range("Ring buffer overwrite exceeds occupied range");
        m_buffer[(m_start + offset) % m_capacity] = data;
    }

    size_t index(size_t base, int offset) const {
        if (m_capacity == 0 || base >= m_capacity) throw std::out_of_range("Invalid ring buffer index");
        const auto distance = static_cast<size_t>(offset < 0 ? -static_cast<long long>(offset) : offset) % m_capacity;
        return offset < 0 ? (base + m_capacity - distance) % m_capacity : (base + distance) % m_capacity;
    }

    T_Data read(size_t offset) const {
        if (offset >= m_count) throw std::out_of_range("Ring buffer read exceeds occupied range");
        return m_buffer[(m_start + offset) % m_capacity];
    }

    size_t read(size_t requested, T_Data *target) const {
        if (requested != 0 && target == nullptr) throw std::invalid_argument("Missing ring buffer destination");
        const size_t count = std::min(requested, m_count);
        if (count == 0) return 0;
        const size_t first = std::min(count, m_capacity - m_start);
        std::memcpy(target, m_buffer + m_start, first * sizeof(T_Data));
        std::memcpy(target + first, m_buffer, (count - first) * sizeof(T_Data));
        return count;
    }

    size_t readAndRemove(size_t requested, T_Data *target) {
        const size_t count = read(requested, target);
        removeBeginning(count);
        return count;
    }

    size_t removeBeginning(size_t requested) {
        const size_t count = std::min(requested, m_count);
        if (count != 0) m_start = (m_start + count) % m_capacity;
        m_count -= count;
        return count;
    }

    size_t size() const { return m_count; }
    size_t capacity() const { return m_capacity; }
    size_t writeIndex() const { return m_writeIndex; }
    size_t start() const { return m_start; }

private:
    T_Data *m_buffer = nullptr;
    size_t m_capacity = 0;
    size_t m_writeIndex = 0;
    size_t m_start = 0;
    size_t m_count = 0;
};

#endif /* ATG_ENGINE_SIM_RING_BUFFER_H */
