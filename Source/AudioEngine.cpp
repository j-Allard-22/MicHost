#include "AudioEngine.h"

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

    if (auto xml = props.getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    knownPlugins.addChangeListener (this);

    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    inputNodeID  = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode))->nodeID;
    outputNodeID = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode))->nodeID;

    restoreChain();

    auto savedDevice = props.getXmlValue ("audioDeviceState");
    deviceManager.initialise (2, 2, savedDevice.get(), true);

    // Prefer 48 kHz on first run: the GoXLR runs at a fixed 48 kHz, and matching
    // rates end-to-end avoids hidden shared-mode resampling in the pipe.
    if (savedDevice == nullptr)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.sampleRate = 48000.0;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    deviceManager.addChangeListener (this);

    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);

    rebuildConnections();
    logCurrentDeviceState ("Engine initialised");
    startTimer (1000);
}

void AudioEngine::shutdown()
{
    if (hasShutDown)
        return;

    hasShutDown = true;

    stopTimer();
    saveState();
    deviceManager.removeChangeListener (this);
    knownPlugins.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    graph.clear();
    deviceManager.closeAudioDevice();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        // Channel counts may have changed (e.g. mono mic -> stereo device).
        rebuildConnections();
        logCurrentDeviceState ("Device change");
    }
    else if (source == &knownPlugins)
    {
        if (auto xml = knownPlugins.createXml())
            props.setValue ("pluginList", xml.get());
    }
}

void AudioEngine::timerCallback()
{
    ++timerTicks;

    // The device counter resets to zero whenever the device reopens; fold it
    // into a monotonic total so soak tests survive unplug/sleep cycles.
    auto current = juce::jmax (0, deviceManager.getXRunCount());

    if (current < lastSeenXRuns)
        lastSeenXRuns = 0;

    if (current > lastSeenXRuns)
    {
        auto delta = current - lastSeenXRuns;
        cumulativeXRuns += delta;
        lastSeenXRuns = current;
        michost::log ("XRuns: +" + juce::String (delta)
                      + " device=" + juce::String (current)
                      + " cumulative=" + juce::String (cumulativeXRuns));
    }

    // Periodic save bounds what a power cut or Task Manager kill can lose:
    // plugin parameter tweaks are only captured by getStateInformation here
    // or at clean shutdown.
    if (timerTicks % 300 == 0)
        saveState();
}

void AudioEngine::logCurrentDeviceState (const juce::String& context) const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        michost::log (context + ": input=\"" + setup.inputDeviceName
                      + "\" output=\"" + setup.outputDeviceName
                      + "\" rate=" + juce::String (device->getCurrentSampleRate(), 0)
                      + " buffer=" + juce::String (device->getCurrentBufferSizeSamples()));
    }
    else
    {
        michost::log (context + ": no audio device open");
    }
}

void AudioEngine::rebuildConnections()
{
    // Async updates coalesce all the removals/re-adds below into a single
    // render-sequence publication, so the audio thread never sees the
    // half-disconnected intermediate topology (which is an audible dropout).
    constexpr auto async = juce::AudioProcessorGraph::UpdateKind::async;

    for (auto& connection : graph.getConnections())
        graph.removeConnection (connection, async);

    int inputChannels = 2;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        inputChannels = device->getActiveInputChannels().countNumberOfSetBits();

    auto connectPair = [this, inputChannels] (juce::AudioProcessorGraph::NodeID source,
                                              bool sourceIsDeviceInput,
                                              juce::AudioProcessorGraph::NodeID destination)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            // A mono mic feeds both channels of the first stage.
            auto sourceChannel = (sourceIsDeviceInput && inputChannels > 0)
                                   ? juce::jmin (ch, inputChannels - 1)
                                   : ch;
            graph.addConnection ({ { source, sourceChannel }, { destination, ch } },
                                 juce::AudioProcessorGraph::UpdateKind::async);
        }
    };

    auto previous = inputNodeID;
    bool previousIsDeviceInput = true;

    for (auto& slot : chain)
    {
        if (slot.unloadedXml != nullptr) // missing plugin: bypass it
            continue;

        connectPair (previous, previousIsDeviceInput, slot.nodeID);
        previous = slot.nodeID;
        previousIsDeviceInput = false;
    }

    connectPair (previous, previousIsDeviceInput, outputNodeID);
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
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 48000.0;
}

bool AudioEngine::inputLooksLikeVirtualCable() const
{
    auto name = deviceManager.getAudioDeviceSetup().inputDeviceName;

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
            juce::Logger::writeToLog ("MicHost: could not restore \"" + desc.name + "\": " + error);
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

    if (auto xml = deviceManager.createStateXml())
        props.setValue ("audioDeviceState", xml.get());

    if (auto xml = knownPlugins.createXml())
        props.setValue ("pluginList", xml.get());

    props.saveIfNeeded();
}

double AudioEngine::preferredSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 48000.0;
}

int AudioEngine::preferredBlockSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    return 512;
}

juce::File AudioEngine::getDeadMansPedalFile() const
{
    return props.getFile().getSiblingFile ("plugin-scan-crash.txt");
}
