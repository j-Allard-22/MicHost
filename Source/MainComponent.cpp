#include "MainComponent.h"

//==============================================================================
class MainComponent::PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& instance,
                  juce::AudioProcessorGraph::NodeID nodeIDToUse,
                  MainComponent& ownerToUse)
        : DocumentWindow (instance.getName(),
                          ownerToUse.getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::closeButton),
          owner (ownerToUse),
          nodeID (nodeIDToUse)
    {
        setUsingNativeTitleBar (true);

        if (auto* editor = instance.createEditorIfNeeded())
            setContentOwned (editor, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (instance), true);

        setResizable (true, false);
        centreAroundComponent (&owner, getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        owner.editorWindows.removeObject (this); // deletes this
    }

    MainComponent& owner;
    const juce::AudioProcessorGraph::NodeID nodeID;
};

//==============================================================================
MainComponent::MainComponent (AudioEngine& engineToUse)
    : engine (engineToUse),
      deviceSelector (engineToUse.deviceManager,
                      1, 2,     // input channels: mono mic up to stereo
                      2, 2,     // output channels: the cable is stereo
                      false, false,
                      true, false)
{
    engine.onChainChanged = [this]
    {
        chainList.updateContent();
        chainList.repaint();
        updateButtons();
    };

    engine.onPluginRemoved = [this] (juce::AudioProcessorGraph::NodeID nodeID)
    {
        closeEditorsForNode (nodeID);
    };

    chainHeading.setText ("Plugin chain (mic -> top to bottom -> cable)", juce::dontSendNotification);
    deviceHeading.setText ("Audio devices (input = your mic, output = CABLE Input)", juce::dontSendNotification);

    for (auto* label : { &chainHeading, &deviceHeading })
        label->setFont (juce::Font (juce::FontOptions (15.0f)));

    addAndMakeVisible (chainHeading);
    addAndMakeVisible (deviceHeading);
    addAndMakeVisible (chainList);
    addAndMakeVisible (deviceSelector);
    addAndMakeVisible (autostartToggle);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (warningLabel);

    for (auto* button : { &addButton, &removeButton, &upButton, &downButton, &editorButton, &managerButton })
        addAndMakeVisible (*button);

    addButton.onClick     = [this] { showAddPluginMenu(); };
    removeButton.onClick  = [this] { engine.removePlugin (chainList.getSelectedRow()); };
    editorButton.onClick  = [this] { openEditorForRow (chainList.getSelectedRow()); };
    managerButton.onClick = [this] { showPluginManager(); };

    upButton.onClick = [this]
    {
        auto row = chainList.getSelectedRow();
        engine.movePlugin (row, -1);
        chainList.selectRow (juce::jmax (0, row - 1));
    };

    downButton.onClick = [this]
    {
        auto row = chainList.getSelectedRow();
        engine.movePlugin (row, 1);
        chainList.selectRow (juce::jmin (engine.getNumPlugins() - 1, row + 1));
    };

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    warningLabel.setJustificationType (juce::Justification::centredLeft);
    warningLabel.setColour (juce::Label::textColourId, juce::Colours::orange);

#if JUCE_WINDOWS
    const juce::String runKey ("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\MicHost");
    const auto runValue = "\""
        + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName()
        + "\" --minimized";

    auto autostartEnabled = juce::WindowsRegistry::valueExists (runKey);

    // Self-heal a stale entry (exe moved/rebuilt elsewhere), otherwise the
    // toggle shows enabled while Windows silently fails to launch it at login.
    if (autostartEnabled && juce::WindowsRegistry::getValue (runKey) != runValue)
        juce::WindowsRegistry::setValue (runKey, runValue);

    autostartToggle.setToggleState (autostartEnabled, juce::dontSendNotification);

    autostartToggle.onClick = [this, runKey, runValue]
    {
        if (autostartToggle.getToggleState())
            juce::WindowsRegistry::setValue (runKey, runValue);
        else
            juce::WindowsRegistry::deleteValue (runKey);
    };
#else
    autostartToggle.setEnabled (false);
#endif

    chainList.setRowHeight (26);
    updateButtons();
    startTimerHz (2);
    setSize (900, 600);
}

MainComponent::~MainComponent()
{
    engine.onChainChanged = nullptr;
    engine.onPluginRemoved = nullptr;

    // The Manage Plugins dialog references the engine's format manager and
    // plugin list; it must die before the engine does (MainWindow is destroyed
    // before AudioEngine::shutdown in the app's shutdown sequence).
    pluginManagerDialog.deleteAndZero();
    editorWindows.clear();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto bottom = area.removeFromBottom (44);
    warningLabel.setBounds (bottom.removeFromTop (22));
    statusLabel.setBounds (bottom);

    auto left = area.removeFromLeft (area.getWidth() / 2);

    chainHeading.setBounds (left.removeFromTop (24));

    auto controls = left.removeFromBottom (100);
    auto row1 = controls.removeFromTop (32);
    addButton.setBounds (row1.removeFromLeft (row1.getWidth() / 3).reduced (2));
    removeButton.setBounds (row1.removeFromLeft (row1.getWidth() / 2).reduced (2));
    editorButton.setBounds (row1.reduced (2));

    auto row2 = controls.removeFromTop (32);
    upButton.setBounds (row2.removeFromLeft (row2.getWidth() / 3).reduced (2));
    downButton.setBounds (row2.removeFromLeft (row2.getWidth() / 2).reduced (2));
    managerButton.setBounds (row2.reduced (2));

    autostartToggle.setBounds (controls.removeFromTop (30).reduced (2));

    chainList.setBounds (left.reduced (2, 4));

    deviceHeading.setBounds (area.removeFromTop (24));
    deviceSelector.setBounds (area.reduced (2, 4));
}

//==============================================================================
int MainComponent::getNumRows()
{
    return engine.getNumPlugins();
}

void MainComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (findColour (juce::TextEditor::highlightColourId));

    juce::String text (juce::String (row + 1) + ".  " + engine.getPluginName (row));
    auto missing = engine.isSlotMissing (row);

    if (missing)
        text << "   (missing - plugin not found, settings preserved)";

    if (auto* instance = engine.getPluginInstance (row))
    {
        auto latency = instance->getLatencySamples();

        if (latency > 0)
            text << "   (+" << juce::String (latency * 1000.0 / engine.getCurrentSampleRate(), 1) << " ms)";
    }

    auto colour = findColour (juce::ListBox::textColourId);
    g.setColour (missing ? colour.withAlpha (0.5f) : colour);
    g.drawText (text, 8, 0, width - 12, height, juce::Justification::centredLeft);
}

void MainComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    openEditorForRow (row);
}

void MainComponent::selectedRowsChanged (int)
{
    updateButtons();
}

//==============================================================================
void MainComponent::timerCallback()
{
    juce::String status;

    if (auto* device = engine.deviceManager.getCurrentAudioDevice())
    {
        auto rate = device->getCurrentSampleRate();
        auto buffer = device->getCurrentBufferSizeSamples();
        auto pluginLatency = engine.getTotalPluginLatencySamples();

        status << juce::String (rate / 1000.0, 1) << " kHz, "
               << buffer << " samples ("
               << juce::String (buffer * 1000.0 / rate, 1) << " ms buffer)"
               << "   |   plugin latency " << juce::String (pluginLatency * 1000.0 / rate, 1) << " ms"
               << "   |   CPU " << juce::String (engine.deviceManager.getCpuUsage() * 100.0, 1) << "%"
               << "   |   xruns " << engine.deviceManager.getXRunCount();
    }
    else
    {
        status = "No audio device open - pick devices on the right.";
    }

    statusLabel.setText (status, juce::dontSendNotification);

    warningLabel.setText (engine.inputLooksLikeVirtualCable()
                            ? "Warning: the selected INPUT is a virtual-cable endpoint - that loops the cable "
                              "into itself. Select your real microphone instead."
                            : juce::String(),
                          juce::dontSendNotification);
}

//==============================================================================
void MainComponent::showAddPluginMenu()
{
    auto types = engine.knownPlugins.getTypes();

    if (types.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                "No plugins scanned yet",
                                                "Use \"Manage Plugins...\" to scan your VST folders first.");
        return;
    }

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton),
                        [this, types] (int result)
                        {
                            auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

                            if (index < 0)
                                return;

                            juce::String error;

                            if (! engine.addPlugin (types.getReference (index), error))
                                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                        "Could not load plugin", error);
                        });
}

void MainComponent::openEditorForRow (int row)
{
    auto* instance = engine.getPluginInstance (row);

    if (instance == nullptr)
        return;

    auto nodeID = engine.getNodeID (row);

    for (auto* window : editorWindows)
    {
        if (window->nodeID == nodeID)
        {
            window->toFront (true);
            return;
        }
    }

    editorWindows.add (new PluginWindow (*instance, nodeID, *this));
}

void MainComponent::closeEditorsForNode (juce::AudioProcessorGraph::NodeID nodeID)
{
    for (int i = editorWindows.size(); --i >= 0;)
        if (editorWindows.getUnchecked (i)->nodeID == nodeID)
            editorWindows.remove (i);
}

void MainComponent::showPluginManager()
{
    if (pluginManagerDialog != nullptr)
    {
        pluginManagerDialog->toFront (true);
        return;
    }

    auto* list = new juce::PluginListComponent (engine.formatManager,
                                                engine.knownPlugins,
                                                engine.getDeadMansPedalFile(),
                                                &engine.getProperties(),
                                                true);
    list->setSize (680, 450);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (list);
    options.dialogTitle = "Manage Plugins";
    options.dialogBackgroundColour = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;

    pluginManagerDialog = options.launchAsync();
}

void MainComponent::updateButtons()
{
    auto row = chainList.getSelectedRow();
    auto haveSelection = juce::isPositiveAndBelow (row, engine.getNumPlugins());

    removeButton.setEnabled (haveSelection);
    editorButton.setEnabled (haveSelection && engine.getPluginInstance (row) != nullptr);
    upButton.setEnabled (haveSelection && row > 0);
    downButton.setEnabled (haveSelection && row < engine.getNumPlugins() - 1);
}
