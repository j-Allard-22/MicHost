#include "AudioEngine.h"
#include "PluginScanning.h"

AudioEngine::AudioEngine (juce::PropertiesFile& propertiesFile)
    : props (propertiesFile)
{
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

void AudioEngine::initialise()
{
    juce::addDefaultFormatsToManager (formatManager);

    // Probe plugin files in a throwaway child process so a crashing plugin
    // can't take the host down mid-scan.
    knownPlugins.setCustomScanner (std::make_unique<CustomPluginScanner>());

    if (auto xml = props.getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    knownPlugins.addChangeListener (this);

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNodeID  = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode))->nodeID;
    outputNodeID = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode))->nodeID;

    restoreChain();

    inputSide.desired  = props.getValue (inputSide.propsKey);
    outputSide.desired = props.getValue (outputSide.propsKey);

    auto inXml  = props.getXmlValue ("audioInputDeviceState");
    auto outXml = props.getXmlValue ("audioOutputDeviceState");

    // One-time migration from the pre-bridge combined-device key: split the
    // saved names across the two managers and seed the desired-device keys.
    if (auto legacy = props.getXmlValue ("audioDeviceState"))
    {
        if (inputSide.desired.isEmpty() && outputSide.desired.isEmpty())
        {
            inputSide.desired  = legacy->getStringAttribute ("audioInputDeviceName");
            outputSide.desired = legacy->getStringAttribute ("audioOutputDeviceName");
        }

        auto makeSideXml = [&legacy] (const juce::String& inputName, const juce::String& outputName)
        {
            auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
            xml->setAttribute ("deviceType", legacy->getStringAttribute ("deviceType"));
            xml->setAttribute ("audioInputDeviceName", inputName);
            xml->setAttribute ("audioOutputDeviceName", outputName);
            return xml;
        };

        if (inXml == nullptr)
            inXml = makeSideXml (inputSide.desired, {});

        if (outXml == nullptr)
            outXml = makeSideXml ({}, outputSide.desired);

        props.removeValue ("audioDeviceState");
        michost::log ("Migrated combined device state to split input/output state");
    }

    // Upgrade path: adopt previously saved names as the desired ones so the
    // watchdog can defend them from now on.
    if (inputSide.desired.isEmpty() && inXml != nullptr)
        inputSide.desired = inXml->getStringAttribute ("audioInputDeviceName");

    if (outputSide.desired.isEmpty() && outXml != nullptr)
        outputSide.desired = outXml->getStringAttribute ("audioOutputDeviceName");

    props.setValue (inputSide.propsKey, inputSide.desired);
    props.setValue (outputSide.propsKey, outputSide.desired);

    // Never silently fall back to default devices once the user has chosen:
    // at login the saved (USB) mic may not have enumerated yet, and "default"
    // would render the processed mic into the speakers while Discord reads
    // silence from the cable. No device + a red tray is the honest failure
    // mode; the watchdog reopens the right device the moment it appears.
    inputDeviceManager.initialise  (2, 0, inXml.get(),  /*selectDefaultDeviceOnFailure*/ inXml == nullptr);
    outputDeviceManager.initialise (0, 2, outXml.get(), /*selectDefaultDeviceOnFailure*/ outXml == nullptr);

    // Prefer 48 kHz on first run: the GoXLR runs at a fixed 48 kHz, and
    // matching rates end-to-end avoids hidden shared-mode resampling.
    for (auto* firstRun : { inXml == nullptr ? &inputDeviceManager : nullptr,
                            outXml == nullptr ? &outputDeviceManager : nullptr })
    {
        if (firstRun != nullptr)
        {
            auto setup = firstRun->getAudioDeviceSetup();
            setup.sampleRate = 48000.0;
            firstRun->setAudioDeviceSetup (setup, true);
        }
    }

    inputDeviceManager.addChangeListener (this);
    outputDeviceManager.addChangeListener (this);

    bridge.setFreezeRatio (props.getBoolValue ("bridgeFreezeRatio", false));

    inputDeviceManager.addAudioCallback (&bridge.getCaptureCallback());
    outputDeviceManager.addAudioCallback (&bridge.getRenderCallback());

    rebuildConnections();
    logCurrentDeviceState ("Engine initialised");
    updateHealth(); // the tray icon reads health right after construction
    startTimer (1000);
}

void AudioEngine::shutdown()
{
    if (hasShutDown)
        return;

    hasShutDown = true;

    stopTimer();
    saveState();
    inputDeviceManager.removeChangeListener (this);
    outputDeviceManager.removeChangeListener (this);
    knownPlugins.removeChangeListener (this);
    inputDeviceManager.removeAudioCallback (&bridge.getCaptureCallback());
    outputDeviceManager.removeAudioCallback (&bridge.getRenderCallback());
    graph.clear();
    inputDeviceManager.closeAudioDevice();
    outputDeviceManager.closeAudioDevice();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &inputDeviceManager || source == &outputDeviceManager)
    {
        auto& side = source == &inputDeviceManager ? inputSide : outputSide;

        logCurrentDeviceState (side.isInput ? "Input device change" : "Output device change");

        // Adopt as the new desired device only when the change is
        // user-attributable: a recent click in a selector or keyboard focus
        // inside one right now - and never shortly after a watchdog action
        // (its own change messages arrive asynchronously).
        auto now = juce::Time::getMillisecondCounter();
        auto userAttributable = (now - lastUserInteractionMs < 10000)
                             || (selectorHasFocus != nullptr && selectorHasFocus());

        if (userAttributable
            && now - lastWatchdogActionMs > 2000
            && side.manager->getCurrentAudioDevice() != nullptr)
        {
            adoptSideAsDesired (side, "user selection");
        }
    }
    else if (source == &knownPlugins)
    {
        if (auto xml = knownPlugins.createXml())
            props.setValue ("pluginList", xml.get());
    }
}

void AudioEngine::accumulateXRuns (juce::AudioDeviceManager& manager, int& lastSeen, const char* label)
{
    // The device counter resets to zero whenever the device reopens; fold it
    // into a monotonic total so soak tests survive unplug/sleep cycles.
    auto current = juce::jmax (0, manager.getXRunCount());

    if (current < lastSeen)
        lastSeen = 0;

    if (current > lastSeen)
    {
        auto delta = current - lastSeen;
        cumulativeXRuns += delta;
        lastSeen = current;
        michost::log ("XRuns (" + juce::String (label) + "): +" + juce::String (delta)
                      + " device=" + juce::String (current)
                      + " cumulative=" + juce::String (cumulativeXRuns));
    }
}

void AudioEngine::timerCallback()
{
    ++timerTicks;

    accumulateXRuns (inputDeviceManager, lastSeenXRunsIn, "input");
    accumulateXRuns (outputDeviceManager, lastSeenXRunsOut, "output");

    // Periodic save bounds what a power cut or Task Manager kill can lose:
    // plugin parameter tweaks are only captured by getStateInformation here
    // or at clean shutdown.
    if (timerTicks % 300 == 0)
        saveState();

    // One telemetry line per minute: steady-state corr IS the measured clock
    // drift of this hardware pair - the soak-test deliverable.
    if (timerTicks % 60 == 0 && bridge.isRunning())
        michost::log ("Bridge: fill=" + juce::String (bridge.getRingFill())
                      + "/" + juce::String (bridge.getRingTarget())
                      + " corr=" + juce::String (bridge.getCorrectionPpm(), 1) + "ppm"
                      + " underruns=" + juce::String (bridge.getUnderruns())
                      + " overruns=" + juce::String (bridge.getOverruns()));

    runDeviceWatchdog();
    updateHealth();
}

void AudioEngine::logCurrentDeviceState (const juce::String& context) const
{
    auto describe = [] (const juce::AudioDeviceManager& manager, bool isInput) -> juce::String
    {
        if (auto* device = const_cast<juce::AudioDeviceManager&> (manager).getCurrentAudioDevice())
        {
            auto setup = manager.getAudioDeviceSetup();
            return "\"" + (isInput ? setup.inputDeviceName : setup.outputDeviceName)
                 + "\" rate=" + juce::String (device->getCurrentSampleRate(), 0)
                 + " buffer=" + juce::String (device->getCurrentBufferSizeSamples());
        }

        return "(none)";
    };

    michost::log (context + ": input=" + describe (inputDeviceManager, true)
                  + " output=" + describe (outputDeviceManager, false));
}

void AudioEngine::adoptSideAsDesired (WatchedSide& side, const juce::String& reason)
{
    auto setup = side.manager->getAudioDeviceSetup();
    auto current = side.isInput ? setup.inputDeviceName : setup.outputDeviceName;

    if (current == side.desired || current.isEmpty())
        return;

    side.desired = current;
    props.setValue (side.propsKey, side.desired);
    michost::log ("Desired " + juce::String (side.isInput ? "input" : "output")
                  + " now \"" + side.desired + "\" (" + reason + ")");
}

void AudioEngine::runDeviceWatchdog()
{
    // A side with no desired name yet (true first run, until the user picks)
    // has no intent to defend - the watchdog stays inert for it.
    for (auto* side : { &inputSide, &outputSide })
        if (side->desired.isNotEmpty())
            reconcileSide (*side);
}

void AudioEngine::reconcileSide (WatchedSide& side)
{
    auto* device = side.manager->getCurrentAudioDevice();
    auto setup = side.manager->getAudioDeviceSetup();
    auto current = side.isInput ? setup.inputDeviceName : setup.outputDeviceName;

    // "Usable" excludes an open-but-dead device reporting a zero rate.
    auto usable = device != nullptr && device->getCurrentSampleRate() > 0.0;

    if (usable && current == side.desired)
    {
        side.backoffSeconds = 5;
        side.nextReconcileTick = 0; // a fresh failure episode retries immediately
        side.loggedWaiting = false;
        return;
    }

    if (timerTicks < side.nextReconcileTick)
        return;

    side.nextReconcileTick = timerTicks + side.backoffSeconds;
    side.backoffSeconds = juce::jmin (15, side.backoffSeconds + 5);

    auto* type = side.manager->getCurrentDeviceTypeObject();
    if (type == nullptr)
        return;

    type->scanForDevices();

    if (! type->getDeviceNames (side.isInput).contains (side.desired))
    {
        // The desired device is gone. If JUCE's own device-loss handling fell
        // back to something else, close it: capturing a mic the user never
        // chose (or blasting the chain out of their speakers) is worse than
        // honest silence and a red tray.
        if (device != nullptr)
        {
            michost::log ("Watchdog: closing non-desired " + juce::String (side.isInput ? "input" : "output")
                          + " \"" + current + "\" while waiting for \"" + side.desired + "\"");
            lastWatchdogActionMs = juce::Time::getMillisecondCounter();
            side.manager->closeAudioDevice();
        }

        if (! side.loggedWaiting)
        {
            michost::log ("Watchdog: waiting for " + juce::String (side.isInput ? "input" : "output")
                          + " \"" + side.desired + "\" to appear");
            side.loggedWaiting = true;
        }

        return;
    }

    auto want = setup;
    (side.isInput ? want.inputDeviceName : want.outputDeviceName) = side.desired;
    want.useDefaultInputChannels  = true;
    want.useDefaultOutputChannels = true;
    want.sampleRate = 48000.0;
    want.bufferSize = 0; // device default

    michost::log ("Watchdog: reopening " + juce::String (side.isInput ? "input" : "output")
                  + " \"" + side.desired + "\"");

    lastWatchdogActionMs = juce::Time::getMillisecondCounter();
    auto error = side.manager->setAudioDeviceSetup (want, true);

    if (error.isNotEmpty())
        michost::log ("Watchdog: reopen failed: " + error);
    else
    {
        michost::log ("Watchdog: reopen succeeded");
        side.backoffSeconds = 5;
        side.loggedWaiting = false;
    }
}

void AudioEngine::updateHealth()
{
    auto previous = health;
    juce::String text;

    auto* inputDevice  = inputDeviceManager.getCurrentAudioDevice();
    auto* outputDevice = outputDeviceManager.getCurrentAudioDevice();

    if (inputDevice == nullptr || outputDevice == nullptr)
    {
        health = Health::noDevice;
        auto& missingSide = inputDevice == nullptr ? inputSide : outputSide;
        text = juce::String (inputDevice == nullptr ? "No input device" : "No output device")
             + (missingSide.desired.isNotEmpty() ? " - waiting for \"" + missingSide.desired + "\""
                                                 : " - open MicHost and pick devices");
    }
    else
    {
        auto inSetup  = inputDeviceManager.getAudioDeviceSetup();
        auto outSetup = outputDeviceManager.getAudioDeviceSetup();
        auto inRate   = inputDevice->getCurrentSampleRate();
        auto outRate  = outputDevice->getCurrentSampleRate();

        auto offDesired = (inputSide.desired.isNotEmpty()  && inSetup.inputDeviceName   != inputSide.desired)
                       || (outputSide.desired.isNotEmpty() && outSetup.outputDeviceName != outputSide.desired);

        if (offDesired)
        {
            health = Health::degraded;
            text = "Wrong device: on \"" + inSetup.inputDeviceName + "\" -> \"" + outSetup.outputDeviceName + "\"";
        }
        else if (inputLooksLikeVirtualCable())
        {
            health = Health::degraded;
            text = "Input is a virtual cable - feedback loop";
        }
        else if (! juce::exactlyEqual (inRate, 48000.0) || ! juce::exactlyEqual (outRate, 48000.0))
        {
            health = Health::degraded;
            text = "Rates " + juce::String (inRate / 1000.0, 1) + "/"
                 + juce::String (outRate / 1000.0, 1) + " kHz (want 48/48)";
        }
        else
        {
            health = Health::ok;
            text = inSetup.inputDeviceName + " -> " + outSetup.outputDeviceName
                 + ", 48 kHz, drift " + juce::String (bridge.getCorrectionPpm(), 0) + " ppm"
                 + ", xruns " + juce::String (cumulativeXRuns);
        }
    }

    healthText = "MicHost: " + text;

    if (health != previous)
        michost::log ("Health: " + juce::String (health == Health::ok ? "OK"
                                  : health == Health::degraded ? "DEGRADED" : "NO DEVICE")
                      + " - " + text);
}

void AudioEngine::rebuildConnections()
{
    // Async updates coalesce all the removals/re-adds below into a single
    // render-sequence publication, so the audio thread never sees the
    // half-disconnected intermediate topology (which is an audible dropout).
    constexpr auto async = juce::AudioProcessorGraph::UpdateKind::async;

    for (auto& connection : graph.getConnections())
        graph.removeConnection (connection, async);

    // The bridge always presents two channels to the graph (mono mics are
    // fanned out in RateMatchedBridge::capture), so wiring is straight 2-ch.
    auto connectPair = [this, async] (juce::AudioProcessorGraph::NodeID source,
                                      juce::AudioProcessorGraph::NodeID destination)
    {
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { source, ch }, { destination, ch } }, async);
    };

    auto previous = inputNodeID;

    for (auto& slot : chain)
    {
        if (slot.unloadedXml != nullptr) // missing plugin: bypass it
            continue;

        connectPair (previous, slot.nodeID);
        previous = slot.nodeID;
    }

    connectPair (previous, outputNodeID);
}

juce::AudioPluginInstance* AudioEngine::getPluginInstance (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) chain.size()))
        return nullptr;

    if (auto node = graph.getNodeForId (chain[(size_t) index].nodeID))
        return dynamic_cast<juce::AudioPluginInstance*> (node->getProcessor());

    return nullptr;
}

juce::String AudioEngine::getPluginName (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) chain.size()))
        return {};

    return chain[(size_t) index].name;
}

juce::AudioProcessorGraph::NodeID AudioEngine::getNodeID (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) chain.size()))
        return {};

    return chain[(size_t) index].nodeID;
}

bool AudioEngine::addPlugin (const juce::PluginDescription& desc, juce::String& errorMessage)
{
    auto instance = formatManager.createPluginInstance (desc, preferredSampleRate(),
                                                        preferredBlockSize(), errorMessage);
    if (instance == nullptr)
        return false;

    auto node = graph.addNode (std::move (instance));
    if (node == nullptr)
    {
        errorMessage = "The graph could not accept the plugin.";
        return false;
    }

    chain.push_back ({ node->nodeID, desc.name, nullptr });
    rebuildConnections();
    saveChain();

    if (onChainChanged)
        onChainChanged();

    return true;
}

bool AudioEngine::isSlotMissing (int index) const
{
    return juce::isPositiveAndBelow (index, (int) chain.size())
        && chain[(size_t) index].unloadedXml != nullptr;
}

void AudioEngine::removePlugin (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) chain.size()))
        return;

    auto& slot = chain[(size_t) index];

    if (slot.unloadedXml == nullptr)
    {
        // Editor windows hold a pointer into the node's processor; close them first.
        if (onPluginRemoved)
            onPluginRemoved (slot.nodeID);

        graph.removeNode (slot.nodeID, juce::AudioProcessorGraph::UpdateKind::async);
    }

    chain.erase (chain.begin() + index);
    rebuildConnections();
    saveChain();

    if (onChainChanged)
        onChainChanged();
}

void AudioEngine::movePlugin (int index, int delta)
{
    auto target = index + delta;

    if (! juce::isPositiveAndBelow (index, (int) chain.size())
        || ! juce::isPositiveAndBelow (target, (int) chain.size()))
        return;

    std::swap (chain[(size_t) index], chain[(size_t) target]);
    rebuildConnections();
    saveChain();

    if (onChainChanged)
        onChainChanged();
}

int AudioEngine::getTotalPluginLatencySamples() const
{
    int total = 0;

    for (int i = 0; i < getNumPlugins(); ++i)
        if (auto* instance = getPluginInstance (i))
            total += instance->getLatencySamples();

    return total;
}

double AudioEngine::getCurrentSampleRate() const
{
    if (auto* device = const_cast<juce::AudioDeviceManager&> (outputDeviceManager).getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 48000.0;
}

bool AudioEngine::inputLooksLikeVirtualCable() const
{
    auto name = inputDeviceManager.getAudioDeviceSetup().inputDeviceName;

    // Broad on purpose: capturing from any virtual-cable endpoint while we
    // render into one is a feedback loop. NVIDIA Broadcast's virtual mic is a
    // legitimate input and matches none of these.
    return name.containsIgnoreCase ("CABLE")               // VB-Cable, CABLE-A/B, 16ch
        || name.containsIgnoreCase ("VB-Audio")
        || name.containsIgnoreCase ("Voicemeeter")
        || name.containsIgnoreCase ("Virtual Audio Cable"); // VAC "Line 1 (Virtual Audio Cable)"
}

void AudioEngine::saveChain()
{
    juce::XmlElement chainXml ("CHAIN");

    for (int i = 0; i < getNumPlugins(); ++i)
    {
        // A missing plugin's saved description + state are written back verbatim
        // (in position), so its settings survive until the plugin reappears.
        if (auto& unloaded = chain[(size_t) i].unloadedXml)
        {
            chainXml.addChildElement (new juce::XmlElement (*unloaded));
            continue;
        }

        auto* instance = getPluginInstance (i);
        if (instance == nullptr)
            continue;

        auto* slot = chainXml.createNewChildElement ("SLOT");
        slot->addChildElement (instance->getPluginDescription().createXml().release());

        juce::MemoryBlock state;
        instance->getStateInformation (state);
        slot->setAttribute ("state", state.toBase64Encoding());
    }

    props.setValue ("chain", &chainXml);
}

void AudioEngine::restoreChain()
{
    auto chainXml = props.getXmlValue ("chain");
    if (chainXml == nullptr)
        return;

    for (auto* slot : chainXml->getChildWithTagNameIterator ("SLOT"))
    {
        juce::PluginDescription desc;
        bool loaded = false;

        for (auto* child : slot->getChildIterator())
            if (desc.loadFromXml (*child))
            {
                loaded = true;
                break;
            }

        if (! loaded)
        {
            // Unparseable slot: keep it as data rather than dropping it.
            chain.push_back ({ {}, "Unknown plugin", std::make_unique<juce::XmlElement> (*slot) });
            continue;
        }

        juce::String error;
        auto instance = formatManager.createPluginInstance (desc, preferredSampleRate(),
                                                            preferredBlockSize(), error);
        if (instance == nullptr)
        {
            michost::log ("Could not restore \"" + desc.name + "\": " + error);
            chain.push_back ({ {}, desc.name, std::make_unique<juce::XmlElement> (*slot) });
            continue;
        }

        juce::MemoryBlock state;
        if (state.fromBase64Encoding (slot->getStringAttribute ("state")) && state.getSize() > 0)
            instance->setStateInformation (state.getData(), (int) state.getSize());

        auto name = desc.name;
        if (auto node = graph.addNode (std::move (instance)))
            chain.push_back ({ node->nodeID, name, nullptr });
    }
}

void AudioEngine::saveState()
{
    saveChain();

    if (auto xml = inputDeviceManager.createStateXml())
        props.setValue ("audioInputDeviceState", xml.get());

    if (auto xml = outputDeviceManager.createStateXml())
        props.setValue ("audioOutputDeviceState", xml.get());

    if (auto xml = knownPlugins.createXml())
        props.setValue ("pluginList", xml.get());

    props.saveIfNeeded();
}

double AudioEngine::preferredSampleRate() const
{
    return getCurrentSampleRate();
}

int AudioEngine::preferredBlockSize() const
{
    if (auto* device = const_cast<juce::AudioDeviceManager&> (outputDeviceManager).getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    return 512;
}

juce::File AudioEngine::getDeadMansPedalFile() const
{
    return props.getFile().getSiblingFile ("plugin-scan-crash.txt");
}
