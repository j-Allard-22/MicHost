#pragma once

#include <JuceHeader.h>

#include "RateMatchedBridge.h"

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

// Owns the audio devices, the processor graph (mic in -> serial plugin chain
// -> cable out) and chain persistence. The mic and the cable are two
// independently-clocked WASAPI endpoints, so they live on two separate
// AudioDeviceManagers joined by a RateMatchedBridge that fans out mono mics,
// rate-matches the clocks and drives the graph from the render side.
// All chain mutations must happen on the message thread; AudioProcessorGraph
// makes topology changes safe against the running audio thread.
class AudioEngine : private juce::ChangeListener,
                    private juce::Timer
{
public:
    explicit AudioEngine (juce::PropertiesFile& propertiesFile);
    ~AudioEngine() override;

    void initialise();
    void shutdown();

    juce::AudioDeviceManager inputDeviceManager, outputDeviceManager;
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

    // The render-side (cable) rate: the rate the graph runs at.
    double getCurrentSampleRate() const;

    const RateMatchedBridge& getBridge() const noexcept { return bridge; }

    // True when the selected input device looks like a virtual-cable endpoint,
    // which would feed the cable back into itself.
    bool inputLooksLikeVirtualCable() const;

    // Total xruns across both devices and across device reopens (each raw
    // counter resets to zero on reopen, useless for soak tests by itself).
    int getCumulativeXRuns() const noexcept { return cumulativeXRuns; }

    // At-a-glance state for the tray icon. "degraded" = running but on the
    // wrong device, at the wrong rate, or capturing from a virtual cable.
    enum class Health { ok, degraded, noDevice };
    Health getHealth() const noexcept { return health; }
    juce::String getHealthText() const { return healthText; }

    // Called by the UI when the user touches a device selector; device-setup
    // changes shortly after are adopted as the new *desired* devices. Changes
    // with any other cause (login race, unplug, fallback) never are.
    void noteUserDeviceInteraction() noexcept { lastUserInteractionMs = juce::Time::getMillisecondCounter(); }

    // Supplied by the app shell: true while the main window is visible.
    std::function<bool()> isUserInteracting;

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

    // Per-side watchdog bookkeeping (input side and output side reconcile
    // independently - the mic can vanish while the cable stays up).
    struct WatchedSide
    {
        juce::AudioDeviceManager* manager;
        juce::String propsKey;      // desired-name persistence key
        juce::String desired;
        bool isInput;
        int nextReconcileTick = 0, backoffSeconds = 5;
        bool loggedWaiting = false;
    };

    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;
    void logCurrentDeviceState (const juce::String& context) const;
    void adoptSideAsDesired (WatchedSide& side, const juce::String& reason);
    void runDeviceWatchdog();
    void reconcileSide (WatchedSide& side);
    void updateHealth();
    void accumulateXRuns (juce::AudioDeviceManager& manager, int& lastSeen, const char* label);
    void rebuildConnections();
    void restoreChain();
    void saveChain();
    double preferredSampleRate() const;
    int preferredBlockSize() const;

    juce::PropertiesFile& props;
    juce::AudioProcessorGraph graph;
    RateMatchedBridge bridge { graph };
    juce::AudioProcessorGraph::NodeID inputNodeID, outputNodeID;
    std::vector<Slot> chain;
    bool hasShutDown = false;

    int lastSeenXRunsIn = 0, lastSeenXRunsOut = 0, cumulativeXRuns = 0, timerTicks = 0;

    WatchedSide inputSide  { &inputDeviceManager,  "desiredInputDevice",  {}, true };
    WatchedSide outputSide { &outputDeviceManager, "desiredOutputDevice", {}, false };
    juce::uint32 lastUserInteractionMs = 0;
    bool reconciling = false;

    Health health = Health::noDevice;
    juce::String healthText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
