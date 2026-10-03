#include "AudioEngine.h"
#include "MainComponent.h"
#include "UserData.h"
#include "api/CommandDispatcher.h"
#include "api/ApiServer.h"
#include "api/EventBroadcaster.h"
#include "api/McpProcess.h"
#include "engine/HistoryManager.h"
#include "ui/SettingsView.h"

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
        dispatcher = std::make_unique<CommandDispatcher> (*engine);
        historyManager = std::make_unique<HistoryManager> (*engine);
        dispatcher->setHistoryManager (historyManager.get());

        // Engine events fan out to the history and (when running) the API server.
        engine->eventSink = [this] (const juce::var& event)
        {
            if (historyManager != nullptr)
                historyManager->onEngineEvent (event);

            if (apiServer != nullptr)
                apiServer->broadcastEvent (event);
        };

        mcpProcess = std::make_unique<McpProcess>();

        if (settings->getBoolValue (SettingsView::mcpEnabledKey, false))
            mcpProcess->start (settings->getIntValue (SettingsView::mcpPortKey, 53218));

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *dispatcher, *mcpProcess);

        if (settings->getBoolValue ("apiEnabled", true))
        {
            const auto port = settings->getIntValue ("apiPort", 53217);
            apiServer = std::make_unique<ApiServer> (*dispatcher);

            if (apiServer->start (port))
            {
                juce::Logger::writeToLog ("API listening on 127.0.0.1:" + juce::String (port));
                eventBroadcaster = std::make_unique<EventBroadcaster> (*engine, *apiServer);
            }
            else
            {
                juce::Logger::writeToLog ("API could not listen on port " + juce::String (port)
                                          + " (already in use?)");
            }
        }
    }

    void shutdown() override
    {
        engine->eventSink = nullptr;   // stop the fan-out before its targets die
        mcpProcess.reset();            // the adapter talks to the API server: kill it first
        eventBroadcaster.reset();
        apiServer.reset();             // stop accepting commands
        mainWindow.reset();            // UI (and plugin editors) before the engine
        historyManager.reset();
        dispatcher.reset();
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
        MainWindow (const juce::String& name, AudioEngine& engine, CommandDispatcher& dispatcher, McpProcess& mcp)
            : DocumentWindow (name, juce::Colour (0xff1d1f23), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (engine, dispatcher, mcp), true);
            setResizable (true, true);
            // Minimum = all topbar controls packed next to each other (view buttons +
            // transport unit + Perf), with a little slack so Perf never hides there;
            // above that the transport unit centers itself.
            setResizeLimits (890, 500, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            setFullScreen (true);   // maximized by default (user requirement)
        }

        void closeButtonPressed() override
        {
            if (auto* main = dynamic_cast<MainComponent*> (getContentComponent()))
                main->confirmQuit();
            else
                JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<CommandDispatcher> dispatcher;
    std::unique_ptr<HistoryManager> historyManager;
    std::unique_ptr<ApiServer> apiServer;
    std::unique_ptr<EventBroadcaster> eventBroadcaster;
    std::unique_ptr<McpProcess> mcpProcess;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (OrchestralDAWApplication)
