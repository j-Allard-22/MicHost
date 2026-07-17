#include <JuceHeader.h>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

#include "AudioEngine.h"
#include "MainComponent.h"

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, AudioEngine& engineToUse, bool startVisible)
        : DocumentWindow (name,
                          juce::Desktop::getInstance().getDefaultLookAndFeel()
                              .findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons),
          engine (engineToUse)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (engine), true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());

        // With --minimized the window must never become visible, or it
        // flashes on screen at every login before the tray hides it.
        if (startVisible)
            setVisible (true);
    }

    // Hide to tray instead of quitting; the tray menu owns the real quit.
    // Hiding is also a natural flush point for any pending plugin tweaks.
    void closeButtonPressed() override
    {
        setVisible (false);
        engine.saveState();
    }

private:
    AudioEngine& engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    explicit TrayIcon (juce::DocumentWindow& windowToControl)
        : window (windowToControl)
    {
        juce::Image icon (juce::Image::ARGB, 64, 64, true);

        {
            juce::Graphics g (icon);
            g.setColour (juce::Colours::mediumseagreen);
            g.fillEllipse (4.0f, 4.0f, 56.0f, 56.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::Font (juce::FontOptions (40.0f)));
            g.drawText ("M", icon.getBounds(), juce::Justification::centred);
        }

        setIconImage (icon, icon);
        setIconTooltip ("MicHost");
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem ("Show MicHost", [this] { showWindow(); });
            menu.addSeparator();
            menu.addItem ("Quit", [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
            menu.showMenuAsync (juce::PopupMenu::Options()); // pops at the mouse position
        }
        else
        {
            showWindow();
        }
    }

private:
    void showWindow()
    {
        window.setVisible (true);
        window.toFront (true);
    }

    juce::DocumentWindow& window;
};

//==============================================================================
class MicHostApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "MicHost"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    // Single instance for the tray app, but let the --list-devices diagnostic
    // run even while the tray instance is up (otherwise JUCE forwards the
    // command line to the running instance and this process exits silently).
    bool moreThanOneInstanceAllowed() override
    {
        return getCommandLineParameters().contains ("--list-devices");
    }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--list-devices"))
        {
            listDevicesAndQuit();
            return;
        }

        fileLogger.reset (juce::FileLogger::createDefaultAppLogger (
            "MicHost", "MicHost.log",
            "MicHost " + getApplicationVersion() + " starting", 128 * 1024));
        juce::Logger::setCurrentLogger (fileLogger.get());
        michost::log ("Launched (command line: \"" + commandLine + "\")");

        juce::PropertiesFile::Options options;
        options.applicationName = "MicHost";
        options.filenameSuffix = ".settings";
        options.folderName = "MicHost";
        options.osxLibrarySubFolder = "Application Support";
        options.millisecondsBeforeSaving = 2000;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        appProperties.setStorageParameters (options);

        engine = std::make_unique<AudioEngine> (*appProperties.getUserSettings());
        engine->initialise();

        auto startMinimised = commandLine.contains ("--minimized")
                           || commandLine.contains ("--minimised");

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, ! startMinimised);
        trayIcon = std::make_unique<TrayIcon> (*mainWindow);
    }

    void shutdown() override
    {
        trayIcon.reset();
        mainWindow.reset();          // closes plugin editor windows before the graph dies

        if (engine != nullptr)
            engine->shutdown();

        engine.reset();
        appProperties.closeFiles();

        if (fileLogger != nullptr)
        {
            michost::log ("Shut down cleanly");
            juce::Logger::setCurrentLogger (nullptr);
            fileLogger.reset();
        }
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        michost::log ("Another instance started (\"" + commandLine + "\")");

        // A duplicate autostart launch must not pop the hidden window.
        if (commandLine.contains ("--minimized") || commandLine.contains ("--minimised"))
            return;

        if (mainWindow != nullptr)
        {
            mainWindow->setVisible (true);
            mainWindow->toFront (true);
        }
    }

private:
    // Diagnostic mode: "MicHost --list-devices" writes all capture/render device
    // names to stdout and MicHost-devices.txt, then exits. Lets the pipe be
    // verified without the GUI.
    void listDevicesAndQuit()
    {
#if JUCE_WINDOWS
        // GUI-subsystem app: stdout already works when piped/redirected (the
        // shell passes handles), but prints nothing on an interactive console
        // unless we attach to the parent's console. Only attach when stdout
        // has no valid handle, so the piped case keeps working.
        auto stdoutHandle = GetStdHandle (STD_OUTPUT_HANDLE);

        if (stdoutHandle == nullptr || stdoutHandle == INVALID_HANDLE_VALUE)
        {
            if (AttachConsole (ATTACH_PARENT_PROCESS))
            {
                FILE* reopened = nullptr;
                freopen_s (&reopened, "CONOUT$", "w", stdout);
            }
        }
#endif

        juce::String report;
        juce::AudioDeviceManager manager;

        for (auto* type : manager.getAvailableDeviceTypes())
        {
            type->scanForDevices();

            report << "== " << type->getTypeName() << " ==" << juce::newLine
                   << "-- capture --" << juce::newLine;

            for (auto& name : type->getDeviceNames (true))
                report << "  " << name << juce::newLine;

            report << "-- render --" << juce::newLine;

            for (auto& name : type->getDeviceNames (false))
                report << "  " << name << juce::newLine;
        }

        std::cout << report << std::flush;

        juce::File::getCurrentWorkingDirectory()
            .getChildFile ("MicHost-devices.txt")
            .replaceWithText (report);

        setApplicationReturnValue (0);
        quit();
    }

    juce::ApplicationProperties appProperties;
    std::unique_ptr<juce::FileLogger> fileLogger;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> trayIcon;
};

//==============================================================================
START_JUCE_APPLICATION (MicHostApplication)
