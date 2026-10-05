#pragma once

#include "AudioEngine.h"
#include "PluginWindow.h"
#include "PluginScanProcess.h"
#include "diagnostics/PerformancePanel.h"
#include "ui/TrackList.h"
#include "ui/AudioChannelList.h"
#include "ui/InstrumentsView.h"
#include "ui/InstrumentEditorView.h"
#include "ui/SettingsView.h"
#include "ui/ExpressionMapEditorView.h"
#include "ui/ThemedLookAndFeel.h"
#include "ui/PlaceholderView.h"
#include "ui/PianoRollView.h"
#include "ui/ArrangementView.h"
#include "ui/HistoryView.h"
#include "ui/TimelineBar.h"
#include "ui/BusyOverlay.h"

// The single-window shell:
//   topbar (menu, domain buttons, transport) / sidebar + content container / status + keyboard.
// Settings overlays the whole UI; everything else swaps inside the content container.
class CommandDispatcher;
class McpProcess;

class MainComponent final : public juce::Component,
                            private juce::MidiKeyboardState::Listener,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent (AudioEngine&, CommandDispatcher&, McpProcess&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Close flow: asks to save when there are unsaved changes, then quits.
    void confirmQuit();

private:
    enum class ContentView { midiRegions, midiEditor, audioRegions, instruments, instrumentEditor, expressionMaps, history };
    enum class Domain { midi, audio };

    //==============================================================================
    void addTrack();
    void createDefaultTrack();
    void removeTrack (AudioEngine::TrackId);
    void selectTrack (AudioEngine::TrackId, bool forceArm);
    void showTrackContextMenu (AudioEngine::TrackId);
    void chooseTrackOutput (AudioEngine::TrackId);
    void autoNameTrackForOutput (AudioEngine::TrackId, AudioEngine::InstrumentId);
    void chooseNewInstrumentFor (AudioEngine::TrackId);
    void openPluginWindow (AudioEngine::InstrumentId);

    void setDomain (Domain);
    void showContent (ContentView);
    void showMainMenu();
    void confirmDiscard (const juce::String& action, std::function<void()> proceed);
    void newProject();
    void loadProjectDialog();
    void saveProject (bool saveAs, std::function<void()> onSaved = nullptr);
    void applyLoadedProject (const juce::File&, bool ok, const juce::String& warnings);
    void updateWindowTitle();
    static juce::File getProjectsDirectory();
    void openSettings();
    void closeSettings();
    void updateViewVisibility();
    void updatePlaceholders();
    void togglePerfPanel();
    void startPluginScan (juce::StringArray args);

    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    //==============================================================================
    AudioEngine& engine;
    CommandDispatcher& commandDispatcher;
    McpProcess& mcpProcess;

    // Topbar
    juce::TextButton menuButton { "Menu" }, midiDomainButton { "Midi" }, audioDomainButton { "Audio" },
                     instrumentsButton { "Instruments" }, historyButton { "History" };
    // The transport unit: a visually grouped panel with the colorized transport
    // buttons, the position readout (bars.beats + time) and the tempo.
    juce::TextButton rtzButton { "|<" }, playButton { "Play" }, recordButton { "Rec" }, loopButton { "Loop" },
                     perfButton { "Perf" };
    juce::Label bpmLabel, positionLabel, timeLabel;
    juce::Rectangle<int> transportPanel;   // painted behind the unit
    int topbarSeparators[2] = { 0, 0 };    // lines between the topbar's groups

    // Sidebar. The track list and the arrangement share one vertical scroll
    // (same Y axis); declared before both.
    sidebar::VerticalScroll trackScroll;
    juce::TextButton collapseButton { "<<" };
    TrackList trackList { engine, trackScroll };
    AudioChannelList channelList { engine };
    int sidebarWidth = 0;            // 0 = not yet computed (defaults to ~15% of the window)
    bool sidebarCollapsed = false;

    struct SidebarResizer final : juce::Component
    {
        explicit SidebarResizer (MainComponent& ownerToUse) : owner (ownerToUse)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        }

        void mouseDown (const juce::MouseEvent&) override { startWidth = owner.sidebarWidth; }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            owner.sidebarWidth = juce::jlimit (150, juce::jmax (200, owner.getWidth() / 2),
                                               startWidth + event.getDistanceFromDragStartX());
            owner.resized();
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff17191c)); }

        MainComponent& owner;
        int startWidth = 0;
    } sidebarResizer { *this };

    // Timeline (the one shared time axis; the bar owns the ruler, markers and readout)
    TimeAxis timeAxis;
    TimelineBar timelineBar { engine, commandDispatcher, timeAxis };

    // Content views
    PlaceholderView audioRegionsView { "Audio regions" };
    ArrangementView arrangementView { engine, commandDispatcher, timeAxis, trackScroll };
    PianoRollView pianoRollView { engine, commandDispatcher, timeAxis };
    InstrumentsView instrumentsView { engine };
    InstrumentEditorView instrumentEditorView { engine, commandDispatcher };
    ExpressionMapEditorView expressionMapView { engine, commandDispatcher };
    HistoryView historyView { commandDispatcher };
    SettingsView settingsView { engine };

    ContentView contentView = ContentView::midiRegions;
    Domain domain = Domain::midi;
    bool settingsOpen = false;

    // Bottom
    juce::Label statusLabel;
    juce::MidiKeyboardState keyboardState;
    juce::MidiKeyboardComponent keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
    PerformanceTracker perfTracker { engine };
    PerformancePanel perfPanel { perfTracker };

    //==============================================================================
    AudioEngine::TrackId selectedTrack = 0;
    int lastEngineRevision = -1;    // topbar widgets follow engine mutations
    std::map<AudioEngine::InstrumentId, std::unique_ptr<PluginWindow>> pluginWindows;

    // Without a TooltipWindow, component tooltips never show (ISSUES.md "Global")
    juce::TooltipWindow tooltipWindow { this, 700 };

    // Covers the window during long operations (sync, project load)
    BusyOverlay busyOverlay { engine.getBusyStatus() };

    juce::File currentProjectFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    std::unique_ptr<PluginScanProcess> pluginScan;
    juce::String scanStatus;
    bool reloadingPluginCache = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
