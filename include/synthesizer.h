#ifndef ATG_ENGINE_SIM_ENGINE_SYNTHESIZER_H
#define ATG_ENGINE_SIM_ENGINE_SYNTHESIZER_H

#include "convolution_filter.h"
#include "leveling_filter.h"
#include "derivative_filter.h"
#include "low_pass_filter.h"
#include "jitter_filter.h"
#include "ring_buffer.h"
#include "butterworth_low_pass_filter.h"

#include <cinttypes>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <vector>

namespace engine_sim_capture { struct FrameAccess; }

class Synthesizer {
    public:
        struct AudioParameters {
            float volume = 1.0f;
            float convolution = 1.0f;
            float dF_F_mix = 0.01f;
            float inputSampleNoise = 0.5f;
            float inputSampleNoiseFrequencyCutoff = 10000.0f;
            float airNoise = 1.0f;
            float airNoiseFrequencyCutoff = 2000.0f;
            float levelerTarget = 30000.0f;
            float levelerMaxGain = 1.9f;
            float levelerMinGain = 0.00001f;
        };

        struct Parameters {
            int inputChannelCount = 1;
            int inputBufferSize = 1024;
            int audioBufferSize = 44100;
            float inputSampleRate = 10000;
            float audioSampleRate = 44100;
            AudioParameters initialAudioParameters;
        };

        struct InputChannel {
            RingBuffer<float> data;
            float *transferBuffer = nullptr;
            double lastInputSample = 0.0f;
        };

        struct ProcessingFilters {
            ConvolutionFilter convolution;
            DerivativeFilter derivative;
            JitterFilter jitterFilter;
            ButterworthLowPassFilter<float> airNoiseLowPass;
            LowPassFilter inputDcFilter;
            ButterworthLowPassFilter<double> antialiasing;
        };

    public:
        Synthesizer();
        ~Synthesizer();

        void initialize(const Parameters &p);
        void initializeImpulseResponse(
            const int16_t *impulseResponse,
            unsigned int samples,
            float volume,
            int index);
        void startAudioRenderingThread();
        void endAudioRenderingThread();
        void destroy();

        // buffer must hold samples elements. The return value excludes zero padding.
        int readAudioOutput(int samples, int16_t *buffer);

        struct BufferStatistics {
            std::uint64_t droppedInput, droppedOutput, zeroFilledOutput;
            std::uint64_t rendered, delivered, submittedGeneration, completedGeneration;
            size_t stagedInput, pendingInput, activeInput, queuedOutput;
            bool rendering;
        };
        BufferStatistics getBufferStatistics() const;
        std::uint64_t getDroppedInputSamples() const;
        std::uint64_t getDroppedOutputSamples() const;

        void writeInput(const double *data);
        void endInputBlock();

        void waitProcessed();

        void audioRenderingThread();
        void renderAudio();

        double getLatency() const;

        int inputDelta(int s1, int s0) const;
        double inputDistance(double s1, double s0) const;

        void setInputSampleRate(double sampleRate);
        double getInputSampleRate() const;

        int16_t renderAudio(int inputOffset);

        double getLevelerGain();
        AudioParameters getAudioParameters();
        void setAudioParameters(const AudioParameters &params);

    //protected:
        ButterworthLowPassFilter<float> m_antialiasing;
        LevelingFilter m_levelingFilter;
        InputChannel *m_inputChannels;
        AudioParameters m_audioParameters;
        int m_inputChannelCount;
        int m_inputBufferSize;
        double m_inputWriteOffset;
        double m_lastInputSampleOffset;

        RingBuffer<int16_t> m_audioBuffer;
        int m_audioBufferSize;

        float m_inputSampleRate;
        float m_audioSampleRate;

        std::thread *m_thread;
        std::atomic<bool> m_run;

        mutable std::mutex m_lock0;
        std::condition_variable m_cv0;

        ProcessingFilters *m_filters;

    private:
        friend struct engine_sim_capture::FrameAccess;
        bool processBlock(bool waitForWork);
        std::vector<std::vector<float>> m_pendingInput;
        std::vector<int16_t> m_renderedAudio;
        AudioParameters m_pendingParameters, m_renderParameters;
        size_t m_stagedFrames = 0, m_pendingFrames = 0, m_activeFrames = 0;
        std::uint64_t m_submittedGeneration = 0, m_completedGeneration = 0, m_pendingGeneration = 0;
        std::uint64_t m_zeroFilledOutput = 0, m_renderedSamples = 0, m_deliveredSamples = 0;
        bool m_rendering = false;
        double m_publishedGain = 1;
        size_t m_nextInputSample = 0;
        double m_nextInputFraction = 0;
        std::uint64_t m_droppedInputSamples = 0;
        std::uint64_t m_droppedOutputSamples = 0;
        std::atomic<bool> m_workerRunning{false};
        std::uint64_t *m_nonFiniteCaptureCount = nullptr;
};

#endif /* ATG_ENGINE_SIM_ENGINE_SYNTHESIZER_H */
