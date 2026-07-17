#pragma once

#include <JuceHeader.h>

#include <atomic>

// Bridges two independently-clocked WASAPI endpoints (the mic's capture
// device and the cable's render device) that JUCE would otherwise pair with a
// drop-on-overflow / zero-pad-on-underflow FIFO and no rate matching.
//
// The capture callback pushes frames into a lock-free SPSC ring (mono mics
// are fanned out to both channels here). The render callback reads the ring
// fill level, runs a slow PI controller that nudges a fractional resampler
// ratio around the nominal inputRate/outputRate, pulls resampled frames
// through per-channel Lagrange interpolators, runs the AudioProcessorGraph on
// the result, and writes to the device. Steady-state ppm IS the measured
// clock drift between the two endpoints.
//
// Real-time rules: the audio-thread paths allocate nothing and take no locks;
// all cross-thread state is atomics. prepare/start/stop happen on the message
// thread (JUCE starts/stops devices there).
class RateMatchedBridge
{
public:
    explicit RateMatchedBridge (juce::AudioProcessor& processorToDrive);

    juce::AudioIODeviceCallback& getCaptureCallback() noexcept { return captureCallback; }
    juce::AudioIODeviceCallback& getRenderCallback()  noexcept { return renderCallback; }

    // Telemetry for the status bar / soak log (any thread).
    int    getRingFill()     const noexcept { return fillAtomic.load (std::memory_order_relaxed); }
    int    getRingTarget()   const noexcept { return targetFill; }
    double getCorrectionPpm() const noexcept { return ppmAtomic.load (std::memory_order_relaxed); }
    int    getUnderruns()    const noexcept { return underruns.load (std::memory_order_relaxed); }
    int    getOverruns()     const noexcept { return overruns.load (std::memory_order_relaxed); }
    bool   isRunning()       const noexcept { return captureRunning.load (std::memory_order_relaxed)
                                                  && renderRunning.load (std::memory_order_relaxed); }

    // Debug escape hatch (props key "bridgeFreezeRatio"): pins the ratio to
    // the nominal rate quotient so the resampler can be isolated from the
    // controller when chasing artifacts.
    void setFreezeRatio (bool shouldFreeze) noexcept { freezeRatio.store (shouldFreeze, std::memory_order_relaxed); }

    // Latency added by the bridge at the fill target, in output samples.
    int getBridgeLatencySamples() const noexcept { return targetFill; }

private:
    //==========================================================================
    struct CaptureCallback final : juce::AudioIODeviceCallback
    {
        explicit CaptureCallback (RateMatchedBridge& b) : bridge (b) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override { bridge.captureAboutToStart (device); }
        void audioDeviceStopped() override                                  { bridge.captureStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                               float* const*, int, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            bridge.capture (inputChannelData, numInputChannels, numSamples);
        }

        RateMatchedBridge& bridge;
    };

    struct RenderCallback final : juce::AudioIODeviceCallback
    {
        explicit RenderCallback (RateMatchedBridge& b) : bridge (b) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override { bridge.renderAboutToStart (device); }
        void audioDeviceStopped() override                                  { bridge.renderStopped(); }

        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            bridge.render (outputChannelData, numOutputChannels, numSamples);
        }

        RateMatchedBridge& bridge;
    };

    //==========================================================================
    void captureAboutToStart (juce::AudioIODevice*);
    void captureStopped();
    void capture (const float* const* inputChannelData, int numInputChannels, int numSamples);

    void renderAboutToStart (juce::AudioIODevice*);
    void renderStopped();
    void render (float* const* outputChannelData, int numOutputChannels, int numSamples);

    void outputSilence (float* const* outputChannelData, int numOutputChannels, int numSamples);
    void resetStreamState();

    //==========================================================================
    static constexpr int ringCapacity = 8192;   // frames; 128 KB total, ~170 ms @48k of worst-case slack
    static constexpr int targetFill   = 1024;   // frames; ~21 ms cushion = 2 shared-mode periods

    juce::AudioProcessor& processor;

    CaptureCallback captureCallback { *this };
    RenderCallback  renderCallback  { *this };

    juce::AbstractFifo fifo { ringCapacity };
    juce::AudioBuffer<float> ring { 2, ringCapacity };

    juce::LagrangeInterpolator interpolator[2];
    juce::AudioBuffer<float> scratch;       // contiguous staging for split FIFO reads
    juce::AudioBuffer<float> processBuffer; // graph I/O
    juce::MidiBuffer midi;

    // Controller state - render thread only.
    enum class Stream { priming, acquiring, locked };
    Stream stream = Stream::priming;
    double fillLpf = targetFill;
    double integrator = 0.0;
    double lastCorrection = 0.0;
    double blocksInAcquisition = 0.0;
    int renderBlockSize = 0;

    // Cross-thread state.
    std::atomic<double> inputRate { 0.0 }, outputRate { 0.0 };
    std::atomic<bool> captureRunning { false }, renderRunning { false };
    std::atomic<bool> graphPrepared { false };
    std::atomic<bool> resetRequest { true };
    std::atomic<bool> freezeRatio { false };
    std::atomic<int> fillAtomic { 0 }, underruns { 0 }, overruns { 0 };
    std::atomic<double> ppmAtomic { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RateMatchedBridge)
};
