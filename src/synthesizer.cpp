#include "../include/synthesizer.h"

#include "../include/utilities.h"
#include "../include/simulation_fault.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

Synthesizer::Synthesizer() {
    m_inputChannels = nullptr;
    m_inputChannelCount = 0;
    m_inputBufferSize = 0;
    m_inputWriteOffset = 0.0;

    m_audioBufferSize = 0;

    m_inputSampleRate = 0.0;
    m_audioSampleRate = 0.0;

    m_lastInputSampleOffset = 0.0;

    m_run = true;
    m_thread = nullptr;
    m_filters = nullptr;
}

Synthesizer::~Synthesizer() {
    destroy();
}

void Synthesizer::initialize(const Parameters &p) {
    if (p.inputChannelCount <= 0 || p.inputBufferSize <= 0 || p.audioBufferSize <= 0
        || !std::isfinite(p.inputSampleRate) || p.inputSampleRate <= 0
        || !std::isfinite(p.audioSampleRate) || p.audioSampleRate <= 0
        || static_cast<double>(p.audioSampleRate) / p.inputSampleRate > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Synthesizer requires positive capacities, channels and finite sample rates");
    }
    destroy();
    m_nextInputSample = 0;
    m_nextInputFraction = 0;
    m_droppedInputSamples = m_droppedOutputSamples = 0;
    m_inputChannelCount = p.inputChannelCount;
    m_inputBufferSize = p.inputBufferSize;
    m_inputWriteOffset = p.inputBufferSize;
    m_audioBufferSize = p.audioBufferSize;
    m_inputSampleRate = p.inputSampleRate;
    m_audioSampleRate = p.audioSampleRate;
    m_audioParameters = p.initialAudioParameters;
    m_pendingParameters = m_renderParameters = m_audioParameters;
    m_stagedFrames = m_pendingFrames = m_activeFrames = 0;
    m_submittedGeneration = m_completedGeneration = m_pendingGeneration = 0;
    m_zeroFilledOutput = m_renderedSamples = m_deliveredSamples = 0;
    m_rendering = false;
    m_pendingInput.assign(p.inputChannelCount, std::vector<float>(p.inputBufferSize));
    m_renderedAudio.resize(p.inputBufferSize);
    m_inputWriteOffset = 0;
    m_lastInputSampleOffset = 0;
    m_run = true;
    m_levelingFilter = LevelingFilter();
    m_publishedGain = m_levelingFilter.getAttenuation();
    m_antialiasing.resetHistory();

    m_audioBuffer.initialize(p.audioBufferSize);
    m_inputChannels = new InputChannel[p.inputChannelCount];
    for (int i = 0; i < p.inputChannelCount; ++i) {
        m_inputChannels[i].transferBuffer = new float[p.inputBufferSize];
        m_inputChannels[i].data.initialize(p.inputBufferSize);
    }

    m_filters = new ProcessingFilters[p.inputChannelCount];
    for (int i = 0; i < p.inputChannelCount; ++i) {
        m_filters[i].airNoise.reset(p.seed, 2, static_cast<std::uint32_t>(i));
        m_filters[i].jitterFilter.setRandomSeed(p.seed, static_cast<std::uint32_t>(i));
        m_filters[i].airNoiseLowPass.setCutoffFrequency(
            m_audioParameters.airNoiseFrequencyCutoff, m_audioSampleRate);

        m_filters[i].derivative.m_dt = 1 / m_audioSampleRate;

        m_filters[i].inputDcFilter.setCutoffFrequency(10.0);
        m_filters[i].inputDcFilter.m_dt = 1 / m_audioSampleRate;

        m_filters[i].jitterFilter.initialize(
            10,
            m_audioParameters.inputSampleNoiseFrequencyCutoff,
            m_audioSampleRate);

        m_filters[i].antialiasing.setCutoffFrequency(1900.0f, m_audioSampleRate);
    }

    m_levelingFilter.p_target = m_audioParameters.levelerTarget;
    m_levelingFilter.p_maxLevel = m_audioParameters.levelerMaxGain;
    m_levelingFilter.p_minLevel = m_audioParameters.levelerMinGain;
    m_antialiasing.setCutoffFrequency(m_audioSampleRate * 0.45f, m_audioSampleRate);

    // The legacy whole-ring zero prefill left equal cursors and no available output.
    // Storage is initialized, while the queue starts empty.
}

void Synthesizer::initializeImpulseResponse(
    const int16_t *impulseResponse,
    unsigned int samples,
    float volume,
    int index)
{
    std::lock_guard<std::mutex> lock(m_lock0);
    if (m_workerRunning || m_rendering || m_pendingFrames != 0) {
        throw std::logic_error("Stop and drain synthesis before replacing an impulse response");
    }
    if (index < 0 || index >= m_inputChannelCount || m_filters == nullptr) {
        throw std::out_of_range("Impulse response channel is outside the initialized synthesizer");
    }
    if ((samples != 0 && impulseResponse == nullptr) || !std::isfinite(volume)) {
        throw std::invalid_argument("Invalid impulse response data or volume");
    }
    unsigned int clippedLength = 0;
    for (unsigned int i = 0; i < samples; ++i) {
        if (std::abs(impulseResponse[i]) > 100) {
            clippedLength = i + 1;
        }
    }

    const unsigned int sampleCount = std::min(10000U, clippedLength);
    m_filters[index].convolution.initialize(sampleCount);
    for (unsigned int i = 0; i < sampleCount; ++i) {
        m_filters[index].convolution.getImpulseResponse()[i] =
            volume * impulseResponse[i] / INT16_MAX;
    }
}

void Synthesizer::startAudioRenderingThread() {
    std::lock_guard<std::mutex> lock(m_lock0);
    if (m_direct) throw std::logic_error("Synchronous synthesis owns its sample clock");
    if (m_thread != nullptr) return;
    if (m_inputChannelCount == 0) throw std::logic_error("Initialize synthesis before starting its worker");
    m_run = true;
    m_workerRunning = true;
    try { m_thread = new std::thread(&Synthesizer::audioRenderingThread, this); }
    catch (...) { m_workerRunning = false; throw; }
}

void Synthesizer::endAudioRenderingThread() {
    std::thread *worker;
    {
        std::lock_guard<std::mutex> lock(m_lock0);
        m_run = false;
        worker = m_thread;
    }
    m_cv0.notify_all();
    if (worker != nullptr) {
        worker->join();
        delete worker;
        std::lock_guard<std::mutex> lock(m_lock0);
        m_thread = nullptr;
    }
}

void Synthesizer::destroy() {
    endAudioRenderingThread();
    m_audioBuffer.destroy();

    for (int i = 0; i < m_inputChannelCount; ++i) {
        if (m_inputChannels != nullptr) {
            m_inputChannels[i].data.destroy();
            delete[] m_inputChannels[i].transferBuffer;
        }
        if (m_filters != nullptr) m_filters[i].convolution.destroy();
    }

    delete[] m_inputChannels;
    delete[] m_filters;

    m_inputChannels = nullptr;
    m_filters = nullptr;

    m_inputChannelCount = 0;
    std::vector<std::vector<float>>().swap(m_pendingInput);
    std::vector<int16_t>().swap(m_renderedAudio);
    m_stagedFrames = m_pendingFrames = m_activeFrames = 0;
    m_nonFiniteCaptureCount = nullptr;
    m_direct = false;
}

int Synthesizer::readAudioOutput(int samples, int16_t *buffer) {
    if (samples < 0 || (samples != 0 && buffer == nullptr)) {
        throw std::invalid_argument("Audio output requires a nonnegative destination extent");
    }
    if (samples == 0) return 0;
    std::unique_lock<std::mutex> lock(m_lock0);

    const int newDataLength = m_audioBuffer.size();
    if (newDataLength >= samples) {
        m_audioBuffer.readAndRemove(samples, buffer);
    }
    else {
        m_audioBuffer.readAndRemove(newDataLength, buffer);
        std::memset(
            buffer + newDataLength,
            0,
            sizeof(int16_t) * ((size_t)samples - newDataLength));
    }
    
    const int samplesConsumed = std::min(samples, newDataLength);

    m_zeroFilledOutput += samples - samplesConsumed;
    m_deliveredSamples += samplesConsumed;
    lock.unlock();
    m_cv0.notify_all();
    return samplesConsumed;
}

void Synthesizer::waitProcessed() {
    if (!m_workerRunning) {
        renderAudio();
        return;
    }
    std::unique_lock<std::mutex> lock(m_lock0);
    const auto target = m_submittedGeneration;
    m_cv0.wait(lock, [this, target] { return m_completedGeneration >= target; });
}

void Synthesizer::writeInput(const double *data) {
    if (m_direct) throw std::logic_error("Synchronous synthesis does not accept queued input");
    if (m_inputChannelCount == 0 || data == nullptr) throw std::invalid_argument("Missing synthesis input");
    for (int i = 0; i < m_inputChannelCount; ++i) {
        if (!std::isfinite(data[i])) throw std::invalid_argument("Nonfinite synthesis input");
    }
    const double step = static_cast<double>(m_audioSampleRate) / getInputSampleRate();
    m_inputWriteOffset += step;
    if (m_inputWriteOffset >= m_inputBufferSize) {
        m_inputWriteOffset = step < m_inputBufferSize
            ? m_inputWriteOffset - m_inputBufferSize
            : std::fmod(m_inputWriteOffset, m_inputBufferSize);
    }
    const double distance = step < m_inputBufferSize
        ? inputDistance(m_inputWriteOffset, m_lastInputSampleOffset) : step;
    const double first = step < m_inputBufferSize
        ? inputDistance(m_nextInputSample, m_lastInputSampleOffset) : m_nextInputFraction;
    const size_t count = distance > 0 && first <= distance
        ? static_cast<size_t>(std::floor(distance - first)) + 1 : 0;
    const bool accepted = count <= m_inputChannels[0].data.capacity() - m_inputChannels[0].data.size();

    // Discard the newest complete resampled block when it cannot fit. Its phase
    // still advances, so a later accepted block resumes at the correct time.
    {
        std::lock_guard<std::mutex> lock(m_lock0);
        if (!accepted) m_droppedInputSamples += count;
        else m_stagedFrames += count;
    }
    for (int i = 0; i < m_inputChannelCount; ++i) {
        auto &channel = m_inputChannels[i];
        if (accepted) {
            double position = first;
            for (size_t sampleIndex = 0; sampleIndex < count; ++sampleIndex, position += 1.0) {
                const double fraction = position / distance;
                const double sample = channel.lastInputSample * (1 - fraction) + data[i] * fraction;
                channel.data.write(m_filters[i].antialiasing.fast_f(static_cast<float>(sample)));
            }
        }
        channel.lastInputSample = data[i];
    }
    m_nextInputSample = (m_nextInputSample + count) % m_inputBufferSize;
    m_nextInputFraction = first + count - distance;
    m_lastInputSampleOffset = m_inputWriteOffset;
}

void Synthesizer::endInputBlock() {
    std::unique_lock<std::mutex> lock(m_lock0);
    if (m_stagedFrames == 0) return;
    const size_t count = m_stagedFrames;
    if (m_pendingFrames != 0 || (!m_run && m_thread != nullptr)) {
        // Keep the already published block immutable. Reject this newest block.
        m_droppedInputSamples += count;
        for (int i = 0; i < m_inputChannelCount; ++i) m_inputChannels[i].data.removeBeginning(count);
    }
    else {
        for (int i = 0; i < m_inputChannelCount; ++i) {
            m_inputChannels[i].data.readAndRemove(count, m_pendingInput[i].data());
        }
        m_pendingFrames = count;
        m_pendingParameters = m_audioParameters;
        m_pendingGeneration = ++m_submittedGeneration;
    }
    m_stagedFrames = 0;
    lock.unlock();
    m_cv0.notify_all();
}

void Synthesizer::audioRenderingThread() {
    while (processBlock(true)) {}
    {
        std::lock_guard<std::mutex> lock(m_lock0);
        m_workerRunning = false;
    }
    m_cv0.notify_all();
}

void Synthesizer::renderAudio() {
    if (m_direct) throw std::logic_error("Synchronous synthesis does not render queued blocks");
    if (m_workerRunning) throw std::logic_error("Synchronous rendering requires a stopped worker");
    processBlock(false);
}

bool Synthesizer::processBlock(bool waitForWork) {
    std::unique_lock<std::mutex> lock(m_lock0);
    if (waitForWork) m_cv0.wait(lock, [this] { return m_pendingFrames != 0 || !m_run; });
    if (m_pendingFrames == 0) return false;
    if (m_rendering) throw std::logic_error("Concurrent synthesis renderers are unsupported");
    const size_t count = m_pendingFrames;
    const auto generation = m_pendingGeneration;
    m_renderParameters = m_pendingParameters;
    for (int i = 0; i < m_inputChannelCount; ++i) {
        std::copy_n(m_pendingInput[i].data(), count, m_inputChannels[i].transferBuffer);
    }
    m_pendingFrames = 0;
    m_activeFrames = count;
    m_rendering = true;
    lock.unlock();

    // The worker owns DSP histories and scratch. Producer/device locks are free.
    applyAudioParameters();
    for (size_t i = 0; i < count; ++i) m_renderedAudio[i] = renderAudio(static_cast<int>(i));

    lock.lock();
    m_renderedSamples += count;
    size_t published = 0;
    while (published < count) {
        const size_t available = m_audioBuffer.capacity() - m_audioBuffer.size();
        if (available == 0) {
            if (!waitForWork || !m_run) {
                // Stopping never waits for a paused consumer. Previously visible
                // output remains readable, while the unpublished suffix is counted.
                m_droppedOutputSamples += count - published;
                break;
            }
            m_cv0.wait(lock, [this] { return !m_run || m_audioBuffer.size() < m_audioBuffer.capacity(); });
            continue;
        }
        const size_t length = std::min(available, count - published);
        for (size_t i = 0; i < length; ++i) m_audioBuffer.write(m_renderedAudio[published + i]);
        published += length;
        m_activeFrames = count - published;
        m_cv0.notify_all();
    }
    m_activeFrames = 0;
    m_publishedGain = m_levelingFilter.getAttenuation();
    m_completedGeneration = generation;
    m_rendering = false;
    lock.unlock();
    m_cv0.notify_all();
    return true;
}

double Synthesizer::getLatency() const {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_audioSampleRate > 0
        ? static_cast<double>(m_stagedFrames + m_pendingFrames + m_activeFrames) / m_audioSampleRate : 0;
}

Synthesizer::BufferStatistics Synthesizer::getBufferStatistics() const {
    std::lock_guard<std::mutex> lock(m_lock0);
    return {m_droppedInputSamples, m_droppedOutputSamples, m_zeroFilledOutput,
        m_renderedSamples, m_deliveredSamples, m_submittedGeneration, m_completedGeneration,
        m_stagedFrames, m_pendingFrames, m_activeFrames, m_audioBuffer.size(), m_rendering};
}

std::uint64_t Synthesizer::getDroppedInputSamples() const { return getBufferStatistics().droppedInput; }
std::uint64_t Synthesizer::getDroppedOutputSamples() const { return getBufferStatistics().droppedOutput; }

double Synthesizer::getInputSampleRate() const {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_inputSampleRate;
}

int Synthesizer::inputDelta(int s1, int s0) const {
    return (s1 < s0)
        ? m_inputBufferSize - s0 + s1
        : s1 - s0;
}

double Synthesizer::inputDistance(double s1, double s0) const {
    return (s1 < s0)
        ? (double)m_inputBufferSize - s0 + s1
        : s1 - s0;
}

void Synthesizer::setInputSampleRate(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || sampleRate > std::numeric_limits<float>::max()
        || static_cast<double>(m_audioSampleRate) / sampleRate > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Input sample rate must be positive and finite");
    }
    std::lock_guard<std::mutex> lock(m_lock0);
    m_inputSampleRate = static_cast<float>(sampleRate);
}

int16_t Synthesizer::renderAudio(int inputSample) {
    float signal = 0;
    for (int i = 0; i < m_inputChannelCount; ++i) {
        signal += renderChannel(i, m_inputChannels[i].transferBuffer[inputSample]);
    }
    return finishSample(signal, false);
}

float Synthesizer::renderChannel(int i, float input) {
    const float airNoise = m_renderParameters.airNoise;
    const float dF_F_mix = m_renderParameters.dF_F_mix;
    const float convAmount = m_renderParameters.convolution;

    const float jitteredSample = m_filters[i].jitterFilter.fast_f(input);
    const float f_in = jitteredSample;
    const float f_dc = m_filters[i].inputDcFilter.fast_f(f_in);
    const float f = f_in - f_dc;
    const float f_p = m_filters[i].derivative.f(f_in);

    const float noise = static_cast<float>(2.0 * m_filters[i].airNoise.uniform() - 1.0);
    const float r = m_filters->airNoiseLowPass.fast_f(noise);
    const float r_mixed = airNoise * r + (1 - airNoise);

    float v_in = f_p * dF_F_mix + f * r_mixed * (1 - dF_F_mix);
    if (std::fpclassify(v_in) == FP_SUBNORMAL) v_in = 0;
    return convAmount * m_filters[i].convolution.f(v_in) + (1 - convAmount) * v_in;
}

int16_t Synthesizer::finishSample(float signal, bool rejectNonfinite) {
    if (rejectNonfinite && !std::isfinite(signal)) throw engine_sim::SimulationFault("nonfinite mixed synthesis signal");
    signal = m_antialiasing.fast_f(signal);

    m_levelingFilter.p_target = m_renderParameters.levelerTarget;
    const float v_leveled = m_levelingFilter.f(signal) * m_renderParameters.volume;
    if (m_nonFiniteCaptureCount != nullptr && !std::isfinite(v_leveled)) {
        ++*m_nonFiniteCaptureCount;
    }
    if (!std::isfinite(v_leveled)) {
        if (rejectNonfinite) throw engine_sim::SimulationFault("nonfinite signal before quantization");
        return 0;
    }
    if (v_leveled >= INT16_MAX) return INT16_MAX;
    if (v_leveled <= INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(std::lround(v_leveled));
}

void Synthesizer::applyAudioParameters() {
    for (int i = 0; i < m_inputChannelCount; ++i) {
        m_filters[i].airNoiseLowPass.setCutoffFrequency(
            m_renderParameters.airNoiseFrequencyCutoff, m_audioSampleRate);
        m_filters[i].jitterFilter.setJitterScale(m_renderParameters.inputSampleNoise);
    }
}

void Synthesizer::beginSynchronous() {
    if (m_direct || m_thread != nullptr || m_inputChannelCount == 0
        || m_stagedFrames != 0 || m_pendingFrames != 0 || m_renderedSamples != 0) {
        throw std::logic_error("Synchronous synthesis requires a fresh prepared instance");
    }
    m_direct = true;
    m_renderParameters = m_audioParameters;
    applyAudioParameters();
}

int16_t Synthesizer::renderNativeFrame(engine_sim::AcousticExcitation input) {
    if (!m_direct) throw std::logic_error("Synchronous synthesis is not prepared");
    if (input.data == nullptr || input.count != static_cast<std::size_t>(m_inputChannelCount)) {
        throw std::invalid_argument("Native synthesis requires one input per prepared channel");
    }
    float signal = 0;
    for (int i = 0; i < m_inputChannelCount; ++i) {
        const float value = static_cast<float>(input.data[i]);
        if (!std::isfinite(value)) throw engine_sim::SimulationFault("nonfinite native synthesis input");
        const float filtered = m_filters[i].antialiasing.fast_f(value);
        if (!std::isfinite(filtered)) throw engine_sim::SimulationFault("nonfinite interpolated synthesis input");
        const float channel = renderChannel(i, filtered);
        if (!std::isfinite(channel)) throw engine_sim::SimulationFault("nonfinite synthesis channel");
        signal += channel;
    }
    const auto sample = finishSample(signal, true);
    ++m_renderedSamples;
    return sample;
}

double Synthesizer::getLevelerGain() const {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_publishedGain;
}

Synthesizer::AudioParameters Synthesizer::getAudioParameters() const {
    std::lock_guard<std::mutex> lock(m_lock0);
    return m_audioParameters;
}

void Synthesizer::setAudioParameters(const AudioParameters &params) {
    std::lock_guard<std::mutex> lock(m_lock0);
    m_audioParameters = params;
    if (m_direct) {
        m_renderParameters = params;
        applyAudioParameters();
    }
}
