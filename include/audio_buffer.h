#ifndef ATG_ENGINE_SIM_AUDIO_BUFFER_H
#define ATG_ENGINE_SIM_AUDIO_BUFFER_H

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <cstring>
#include <stdint.h>

class AudioBuffer {
    public:
        AudioBuffer();
        ~AudioBuffer();

        void initialize(int sampleRate, int bufferSize);
        void destroy();

        inline double offsetToTime(int offset) const {
            return offset * m_offsetToSeconds;
        }

        inline double timeDelta(int offset0, int offset1) const {
            if (offset1 == offset0) return 0;
            else if (offset1 < offset0) {
                return offsetToTime((m_bufferSize - offset0) + offset1);
            }
            else {
                return offsetToTime(offset1 - offset0);
            }
        }

        inline int offsetDelta(int offset0, int offset1) const {
            if (offset1 == offset0) return 0;
            else if (offset1 < offset0) {
                return (m_bufferSize - offset0) + offset1;
            }
            else {
                return offset1 - offset0;
            }
        }

        inline void writeSample(int16_t sample, int offset, int index = 0) {
            m_samples[getBufferIndex(offset, index)] = sample;
        }

        inline int16_t readSample(int offset, int index) const {
            return m_samples[getBufferIndex(offset, index)];
        }

        inline void commitBlock(int length) {
            if (length < 0 || length > m_bufferSize) throw std::out_of_range("Audio commit exceeds buffer capacity");
            m_writePointer = getBufferIndex(m_writePointer, length);
        }

        inline int getBufferIndex(int offset, int index = 0) const {
            if (m_bufferSize <= 0) throw std::logic_error("Audio buffer is not initialized");
            const auto position = static_cast<long long>(offset) + index;
            return static_cast<int>((position % m_bufferSize + m_bufferSize) % m_bufferSize);
        }

        // The destination must hold length samples, including a full ring.
        inline void copyBuffer(int16_t *dest, int offset, int length) {
            if (length < 0 || length > m_bufferSize) throw std::out_of_range("Audio copy exceeds buffer capacity");
            if (length == 0) return;
            if (dest == nullptr) throw std::invalid_argument("Missing audio destination");
            const int start = getBufferIndex(offset);
            const int first = std::min(length, m_bufferSize - start);
            std::memcpy(dest, m_samples + start, first * sizeof(int16_t));
            std::memcpy(dest + first, m_samples, (length - first) * sizeof(int16_t));
        }

        bool checkForDiscontinuitiy(int threshold) const;

        int m_writePointer;

    protected:
        int m_sampleRate;
        int16_t *m_samples;
        int m_bufferSize;

        double m_offsetToSeconds;
};

#endif /* ATG_ENGINE_SIM_AUDIO_BUFFER_H */
