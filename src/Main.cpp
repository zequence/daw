#include "AudioEngine.h"
#include "MainComponent.h"
#include "UserData.h"

class OrchestralDAWApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        UserData::getAppLog().deleteFile();
        logger = std::make_unique<juce::FileLogger> (UserData::getAppLog(),
                                                     getApplicationName() + " " + getApplicationVersion());
        juce::Logger::setCurrentLogger (logger.get());
        juce::Logger::writeToLog ("User data dir: " + UserData::getDir().getFullPathName());

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        settings = std::make_unique<juce::PropertiesFile> (UserData::getSettingsFile(), options);

        engine = std::make_unique<AudioEngine> (*settings);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);
    }

    void shutdown() override
    {
        mainWindow.reset();   // UI (and plugin editors) before the engine
        engine.reset();
        settings.reset();

        juce::Logger::writeToLog ("Shutdown complete");
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override                    { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, AudioEngine& engine)
            : DocumentWindow (name, juce::Colour (0xff1d1f23), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (engine), true);
            setResizable (true, true);
            setResizeLimits (900, 400, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (OrchestralDAWApplication)
