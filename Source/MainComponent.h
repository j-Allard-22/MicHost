// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jonathan Allard

#pragma once

#include <JuceHeader.h>
#include "AudioEngine.h"

class MainComponent : public juce::Component,
                      private juce::ListBoxModel,
                      private juce::Timer
{
public:
    explicit MainComponent (AudioEngine& engineToUse);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override;

    void timerCallback() override;

    void showAddPluginMenu();
    void openEditorForRow (int row);
    void closeEditorsForNode (juce::AudioProcessorGraph::NodeID nodeID);
    void showPluginManager();
    void updateButtons();

    class PluginWindow;

    AudioEngine& engine;

    juce::ListBox chainList { "chain", this };
    juce::TextButton addButton { "Add..." }, removeButton { "Remove" },
                     upButton { "Move Up" }, downButton { "Move Down" },
                     editorButton { "Open Editor" }, managerButton { "Manage Plugins..." };
    juce::AudioDeviceSelectorComponent inputSelector, outputSelector;
    juce::ToggleButton autostartToggle { "Start with Windows (minimised to tray)" };
    juce::Label chainHeading, inputHeading, outputHeading, statusLabel, warningLabel;

    juce::OwnedArray<PluginWindow> editorWindows;
    juce::Component::SafePointer<juce::DialogWindow> pluginManagerDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
