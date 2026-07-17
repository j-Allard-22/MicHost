// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jonathan Allard

#include "RateMatchedBridge.h"

RateMatchedBridge::RateMatchedBridge (juce::AudioProcessor& processorToDrive)
    : processor (processorToDrive)
{
}

//==============================================================================
// Capture side (mic device thread)

void RateMatchedBridge::captureAboutToStart (juce::AudioIODevice* device)
{
    inputRate.store (device->getCurrentSampleRate(), std::memory_order_relaxed);
    resetRequest.store (true, std::memory_order_release);
    captureRunning.store (true, std::memory_order_release);
}

void RateMatchedBridge::captureStopped()
{
    captureRunning.store (false, std::memory_order_release);
    resetRequest.store (true, std::memory_order_release);
}

void RateMatchedBridge::capture (const float* const* inputChannelData, int numInputChannels, int numSamples)
{
    if (numInputChannels <= 0 || numSamples <= 0)
        return;

    if (fifo.getFreeSpace() < numSamples)
    {
        // The consumer stalled or the controller hasn't locked yet: dropping
        // the incoming block (rather than shuffling the read side, which the
        // SPSC contract forbids from this thread) keeps this branch trivially
        // safe. The render side re-centers.
        overruns.fetch_add (1, std::memory_order_relaxed);
        resetRequest.store (true, std::memory_order_release);
        return;
    }

    // A mono mic feeds both bridge channels; stereo mics map 1:1.
    const auto* left  = inputChannelData[0];
    const auto* right = inputChannelData[numInputChannels > 1 ? 1 : 0];

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

    if (size1 > 0)
    {
        ring.copyFrom (0, start1, left,  size1);
        ring.copyFrom (1, start1, right, size1);
    }

    if (size2 > 0)
    {
        ring.copyFrom (0, start2, left + size1,  size2);
        ring.copyFrom (1, start2, right + size1, size2);
    }

    fifo.finishedWrite (size1 + size2);
}

//==============================================================================
// Render side (cable device thread; also drives the plugin graph)

void RateMatchedBridge::renderAboutToStart (juce::AudioIODevice* device)
{
    auto rate = device->getCurrentSampleRate();
    renderBlockSize = device->getCurrentBufferSizeSamples();

    outputRate.store (rate, std::memory_order_relaxed);

    // Worst plausible consumption per block: 96k-in vs 44.1k-out plus
    // correction headroom; 3x is comfortably above that.
    scratch.setSize (2, renderBlockSize * 3 + 32, false, false, true);
    processBuffer.setSize (2, renderBlockSize, false, false, true);

    // JUCE starts devices from the message thread, so preparing the graph
    // here is the same pattern AudioProcessorPlayer uses.
    processor.setPlayConfigDetails (2, 2, rate, renderBlockSize);
    processor.prepareToPlay (rate, renderBlockSize);
    graphPrepared.store (true, std::memory_order_release);

    resetRequest.store (true, std::memory_order_release);
    renderRunning.store (true, std::memory_order_release);
}

void RateMatchedBridge::renderStopped()
{
    renderRunning.store (false, std::memory_order_release);

    if (graphPrepared.exchange (false))
        processor.releaseResources();
}

void RateMatchedBridge::outputSilence (float* const* outputChannelData, int numOutputChannels, int numSamples)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
}

void RateMatchedBridge::resetStreamState()
{
    // Drain from the consumer side only - AbstractFifo::reset() is not safe
    // against a concurrently-writing producer.
    fifo.finishedRead (fifo.getNumReady());

    for (auto& i : interpolator)
        i.reset();

    stream = Stream::priming;
    fillLpf = targetFill;
    integrator = 0.0;
    lastCorrection = 0.0;
    blocksInAcquisition = 0.0;
}

void RateMatchedBridge::render (float* const* outputChannelData, int numOutputChannels, int numSamples)
{
    outputSilence (outputChannelData, numOutputChannels, numSamples);

    auto inRate  = inputRate.load (std::memory_order_relaxed);
    auto outRate = outputRate.load (std::memory_order_relaxed);

    if (numSamples <= 0 || inRate <= 0.0 || outRate <= 0.0
        || ! captureRunning.load (std::memory_order_acquire)
        || numSamples > processBuffer.getNumSamples())
        return;

    if (resetRequest.exchange (false, std::memory_order_acq_rel))
        resetStreamState();

    const auto fill = fifo.getNumReady();
    fillAtomic.store (fill, std::memory_order_relaxed);

    if (stream == Stream::priming)
    {
        if (fill < targetFill)
            return; // stay silent until the cushion exists

        stream = Stream::acquiring;
        blocksInAcquisition = 0.0;
        fillLpf = fill;
    }

    // --- Controller: fill error (1 s low-pass) -> PI -> ratio correction ---
    const auto blockSeconds = numSamples / outRate;
    const auto lpfAlpha = juce::jlimit (0.0, 1.0, blockSeconds / 1.0);
    fillLpf += lpfAlpha * (fill - fillLpf);

    const auto error = (fillLpf - targetFill) / (double) targetFill;

    const auto acquiring = stream == Stream::acquiring;
    const auto kp = acquiring ? 5.0e-3 : 1.0e-3;
    const auto ki = acquiring ? 5.0e-5 : 1.0e-5;

    integrator = juce::jlimit (-1.0e-3, 1.0e-3, integrator + ki * error);

    auto correction = juce::jlimit (-5.0e-3, 5.0e-3, kp * error + integrator);

    // Slew limit so the controller can never produce audible pitch wobble.
    constexpr auto maxSlewPerBlock = 25.0e-6;
    correction = lastCorrection + juce::jlimit (-maxSlewPerBlock, maxSlewPerBlock,
                                                correction - lastCorrection);
    lastCorrection = correction;

    if (acquiring)
    {
        blocksInAcquisition += blockSeconds;

        if (blocksInAcquisition > 10.0)
            stream = Stream::locked;
    }

    auto ratio = (inRate / outRate) * (1.0 + correction);

    if (freezeRatio.load (std::memory_order_relaxed))
        ratio = inRate / outRate;

    ppmAtomic.store (correction * 1.0e6, std::memory_order_relaxed);

    // --- Pull resampled frames through the interpolators ---
    const auto neededInput = (int) std::ceil (numSamples * ratio) + 8;

    if (neededInput > scratch.getNumSamples())
        return; // config drifted past allocation; silence beats UB

    if (fill < neededInput)
    {
        // Underrun: emit silence and rebuild the cushion before resuming.
        underruns.fetch_add (1, std::memory_order_relaxed);
        stream = Stream::priming;
        return;
    }

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (neededInput, start1, size1, start2, size2);

    for (int ch = 0; ch < 2; ++ch)
    {
        if (size1 > 0) scratch.copyFrom (ch, 0, ring, ch, start1, size1);
        if (size2 > 0) scratch.copyFrom (ch, size1, ring, ch, start2, size2);
    }

    // Identical state + identical (ratio, counts) per channel means both
    // interpolators consume the same number of input samples.
    const auto consumed = interpolator[0].process (ratio, scratch.getReadPointer (0),
                                                   processBuffer.getWritePointer (0), numSamples);
    const auto consumedR = interpolator[1].process (ratio, scratch.getReadPointer (1),
                                                    processBuffer.getWritePointer (1), numSamples);
    jassertquiet (consumed == consumedR);

    fifo.finishedRead (juce::jmin (consumed, size1 + size2));

    // --- Plugin chain ---
    if (graphPrepared.load (std::memory_order_acquire))
    {
        juce::AudioBuffer<float> view (processBuffer.getArrayOfWritePointers(), 2, numSamples);
        midi.clear();
        processor.processBlock (view, midi);
    }

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::copy (outputChannelData[ch],
                                               processBuffer.getReadPointer (juce::jmin (ch, 1)),
                                               numSamples);
}
