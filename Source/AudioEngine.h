#pragma once

#include <JuceHeader.h>

namespace michost
{
    // All diagnostic lines carry a wall-clock stamp so soak-test logs can be
    // correlated with what the user heard (FileLogger itself doesn't stamp).
    inline void log (const juce::String& message)
    {
        juce::Logger::writeToLog (juce::Time::getCurrentTime()
                                      .formatted ("%Y-%m-%d %H:%M:%S ") + message);
    }
}

// Owns the audio device, the processor graph (mic in -> serial plugin chain ->
// cable out) and chain persistence. All chain mutations must happen on the
// message thread; AudioProcessorGraph makes topology changes safe against the
// running audio thread.
class AudioEngine : private juce::ChangeListener,
                    private juce::Timer
{
public:
    explicit AudioEngine (juce::PropertiesFile& propertiesFile);
    ~AudioEngine() override;

    void initialise();
    void shutdown();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    int getNumPlugins() const noexcept { return (int) chain.size(); }
    juce::AudioPluginInstance* getPluginInstance (int index) const;
    juce::String getPluginName (int index) const;
    juce::AudioProcessorGraph::NodeID getNodeID (int index) const;

    // A slot whose plugin could not be instantiated (uninstalled / file moved).
    // Its saved description and state are preserved and re-written on save, so
    // nothing is lost if the plugin comes back later.
    bool isSlotMissing (int index) const;

    bool addPlugin (const juce::PluginDescription& desc, juce::String& errorMessage);
    void removePlugin (int index);
    void movePlugin (int index, int delta);

    int getTotalPluginLatencySamples() const;
    double getCurrentSampleRate() const;

    // Total xruns across device reopens (the raw device counter resets to zero
    // every time the device restarts, which makes it useless for soak tests).
    int getCumulativeXRuns() const noexcept { return cumulativeXRuns; }

    // True when the selected input device looks like a virtual-cable endpoint,
    // which would feed the cable back into itself.
    bool inputLooksLikeVirtualCable() const;

    void saveState();
    juce::File getDeadMansPedalFile() const;
    juce::PropertiesFile& getProperties() noexcept { return props; }

    std::function<void()> onChainChanged;                                     // message thread
    std::function<void (juce::AudioProcessorGraph::NodeID)> onPluginRemoved;  // called BEFORE the node dies

private:
    struct Slot
    {
        juce::AudioProcessorGraph::NodeID nodeID;
        juce::String name;
        std::unique_ptr<juce::XmlElement> unloadedXml; // non-null = missing plugin; holds the saved SLOT verbatim
    };

    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;
    void logCurrentDeviceState (const juce::String& context) const;
    void rebuildConnections();
    void restoreChain();
    void saveChain();
    double preferredSampleRate() const;
    int preferredBlockSize() const;

    juce::PropertiesFile& props;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::NodeID inputNodeID, outputNodeID;
    std::vector<Slot> chain;
    bool hasShutDown = false;

    int lastSeenXRuns = 0, cumulativeXRuns = 0, timerTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
