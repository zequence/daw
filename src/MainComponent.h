#pragma once

#include "AudioEngine.h"
#include "UserData.h"
#include "PluginWindow.h"
#include "ui/AddTrackDialog.h"
#include "PluginScanProcess.h"
#include "diagnostics/PerformancePanel.h"
#include "ui/TrackList.h"
#include "ui/InstrumentsView.h"
#include "ui/InstrumentEditorView.h"
#include "ui/SettingsView.h"
#include "ui/ExpressionMapEditorView.h"
#include "ui/TrackOutputPanel.h"
#include "ui/ThemedLookAndFeel.h"
#include "ui/PlaceholderView.h"
#include "ui/PianoRollView.h"
#include "ui/ArrangementView.h"
#include "ui/HistoryView.h"
#include "ui/TimelineBar.h"
#include "ui/BusyOverlay.h"
#include "ui/MixerView.h"

// The single-window shell:
//   sidebar (menu, tracks) + content container + side pane / the transport bar
//   (right, top or bottom: the main menu) / status.
// Settings overlays the whole UI; everything else swaps inside the content container.
class CommandDispatcher;
class McpProcess;

class MainComponent final : public juce::Component,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    MainComponent (AudioEngine&, CommandDispatcher&, McpProcess&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // the docked editor's border
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Close flow: asks to save when there are unsaved changes, then quits.
    void confirmQuit();

private:
    enum class ContentView { midiRegions, instrumentEditor, expressionMaps };   // (the MIDI editor and the mixer: the panel under the arrangement)   // (the MIDI editor: a panel under the arrangement)   // Instruments and History: the side pane

    // Instruments and History open as a pane on the right, beside the arrangement or the editor
    enum class SidePane { none, instruments, history };
    SidePane sidePane = SidePane::none;
    SidePane lastSidePane = SidePane::instruments;   // what the expand button opens
    void toggleSidePane (SidePane);
    void zoomTrackHeight (int direction);   // +1 taller, -1 shorter (saved in the settings)
    ContentView mainView = ContentView::midiRegions;   // where the instrument / map editors go back to

    //==============================================================================
    void addTrack();
    void createDefaultTrack();
    void removeTrack (AudioEngine::TrackId);
    void selectTrack (AudioEngine::TrackId, bool forceArm);
    void showTrackContextMenu (AudioEngine::TrackId);
    void chooseTrackOutput (AudioEngine::TrackId);
    void showTrackOutputConfig (AudioEngine::TrackId);
    void autoNameTrackForOutput (AudioEngine::TrackId, AudioEngine::InstrumentId);
    void chooseNewInstrumentFor (AudioEngine::TrackId);
    void openPluginWindow (AudioEngine::InstrumentId);
    void addTracks (const AddTrackDialog::Choice&);
    void openInsertWindow (AudioEngine::AudioChannelId, int slot);
    void closeInsertWindows (AudioEngine::AudioChannelId, int slot);   // slot -1: all
    void openTrackPluginWindow (AudioEngine::TrackId);   // toggles the track's (first) instrument GUI

    void showContent (ContentView);
    void showMainMenu();
    void confirmDiscard (const juce::String& action, std::function<void()> proceed);
    void newProject();
    void loadProjectDialog();
    void saveProject (bool saveAs, std::function<void()> onSaved = nullptr);
    void applyLoadedProject (const juce::File&, bool ok, const juce::String& warnings);
    void updateWindowTitle();
    static juce::File getProjectsDirectory();
    void removeInstrumentAsking (AudioEngine::InstrumentId);
    bool triggerArticulation (const std::function<std::optional<ExpressionMap::Target> (const ExpressionMap&)>& find);
    static juce::File startupProjectFile()    { return UserData::getDir().getChildFile ("Startup.odaw"); }
    void openSettings();
    void closeSettings();
    void updateViewVisibility();
    void togglePerfPanel();
    void toggleEditor (bool draw);
    void openEditorOn (std::vector<AudioEngine::TrackId> tracks, AudioEngine::TrackId edited = 0);   // edited: that one (else the top one)
    std::vector<AudioEngine::TrackId> tracksToEdit() const;         // what E/D open (see the .cpp)
    std::vector<AudioEngine::TrackId> inSidebarOrder (const std::set<AudioEngine::TrackId>&) const;
    bool lastSelectionInArrangement = false;                         // which selection E/D follow
    void startPluginScan (juce::StringArray args);

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    //==============================================================================
    AudioEngine& engine;
    CommandDispatcher& commandDispatcher;
    McpProcess& mcpProcess;

    // Topbar
    juce::TextButton menuButton { "Menu" },
                     instrumentsButton { "Instruments" }, historyButton { "History" };   // right side: the side pane
    // The transport unit: a visually grouped panel with the colorized transport
    // buttons, the position readout (bars.beats + time) and the tempo.
    juce::TextButton rtzButton { "|<" }, playButton { "Play" }, recordButton { "Rec" }, loopButton { "Loop" },
                     perfButton { "Perf" }, snapButton { "Snap" }, returnOnStopButton { juce::String::fromUTF8 ("\xe2\x86\xa9") };   // the stop mode
    juce::Label bpmLabel, positionLabel, timeLabel;
    enum class TransportPlace { top, bottom, right };
    TransportPlace transportPlace = TransportPlace::right;   // where the transport bar is (the main menu; saved in the settings)
    void setTransportPlace (TransportPlace);
    juce::TextButton sidePaneButton;       // the right bar's top: expands the side pane (its tabs: Instruments, History)
    mixer::LevelMeter masterMeter { false, true, true };   // the audio out (the master bus), on the right bar: a dB scale and readout
    juce::Rectangle<int> rightBar;         // the transport bar on the right (painted)
    juce::Rectangle<int> sidebarArea;      // the sidebar, its top strip included (painted)
    juce::Rectangle<int> paneTabs;         // the side pane's tab row (painted)
    int sidePaneEdge = -1;                 // the side pane's left edge (a line is painted there)
    int sidePaneWidth = 0;                 // 0 = the default (about a third of the window); saved in the settings

    // The MIDI editor docked under the arrangement: a thin handle at the bottom - click to open or
    // close it, drag to set its height - so the arrangement and the editor show together
    bool editorDocked = false;
    int dockHeight = 0;                    // 0 = the default (about 40% of the content); saved in the settings
    static constexpr int dockHandleHeight = 15;   // room for its tabs (MIDI, MIXER)
    // The panel under the arrangement holds a page: the MIDI editor or the mixer (its tabs on the handle)
    enum class DockPage { editor, mixer };
    DockPage dockPage = DockPage::editor;
    bool isDockOpen() const                 { return editorDocked && contentView == ContentView::midiRegions; }
    bool isEditorDockedShowing() const      { return isDockOpen() && dockPage == DockPage::editor; }
    bool isEditorShowing() const            { return isEditorDockedShowing(); }
    bool isMixerShowing() const             { return isDockOpen() && dockPage == DockPage::mixer; }
    void showDockPage (DockPage);           // opens the panel on that page
    void setAutomationMode (bool);          // F2: the MIDI tracks hidden (sidebar::automationModeSetting)
    void setEditorDocked (bool);

    struct DockHandle final : juce::Component, juce::SettableTooltipClient
    {
        explicit DockHandle (MainComponent& o) : owner (o)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        }

        // Over a tab: its page and its keys
        juce::String getTooltip() override
        {
            const auto position = getMouseXYRelative();
            const auto keysOf = [] (const char* id) { return keys::Bindings::describe (keys::Bindings::get().keysFor (id)); };

            if (tabArea (DockPage::editor).contains (position))
                return "MIDI editor (" + keysOf ("view.midiEditor") + ")";

            if (tabArea (DockPage::mixer).contains (position))
                return "Mixer (" + keysOf ("view.mixer") + ")";

            return {};
        }

        // The tabs, small, at the handle's left
        juce::Rectangle<int> tabArea (DockPage page) const
        {
            return { page == DockPage::editor ? 8 : 52, 1, 42, getHeight() - 2 };
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (isMouseOver() ? 0xff2c3036 : 0xff24272c));
            const auto grip = juce::Rectangle<float> (36.0f, 3.0f).withCentre (getLocalBounds().toFloat().getCentre());
            g.setColour (juce::Colours::white.withAlpha (owner.editorDocked ? 0.5f : 0.35f));
            g.fillRoundedRectangle (grip, 1.5f);

            g.setFont (juce::FontOptions (9.5f, juce::Font::bold).withKerningFactor (0.08f));

            for (auto [page, label] : { std::pair (DockPage::editor, "MIDI"), std::pair (DockPage::mixer, "MIXER") })
            {
                const auto open = owner.isDockOpen() && owner.dockPage == page;
                const auto tab = tabArea (page).toFloat();

                if (open)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.12f));
                    g.fillRoundedRectangle (tab, 2.5f);
                }

                g.setColour (juce::Colours::white.withAlpha (open ? 0.9f : 0.45f));
                g.drawText (label, tab, juce::Justification::centred, false);
            }
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override  { repaint(); }

        void mouseDown (const juce::MouseEvent&) override
        {
            startHeight = owner.editorDocked ? owner.currentDockHeight() : 0;
            dragged = false;
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (std::abs (event.getDistanceFromDragStartY()) < 3 && ! dragged)
                return;

            dragged = true;
            const auto wanted = startHeight - event.getDistanceFromDragStartY();

            if (wanted < 80)   // dragged (nearly) shut
            {
                owner.setEditorDocked (false);
                return;
            }

            owner.dockHeight = juce::jlimit (80, juce::jmax (100, owner.maxDockHeight()), wanted);
            owner.setEditorDocked (true);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (! dragged)
            {
                auto onTab = false;

                for (auto page : { DockPage::editor, DockPage::mixer })
                    if (tabArea (page).contains (event.getPosition()))
                    {
                        onTab = true;

                        if (owner.isDockOpen() && owner.dockPage == page)
                            owner.setEditorDocked (false);   // its tab again: closes
                        else
                            owner.showDockPage (page);
                    }

                if (! onTab)
                    owner.setEditorDocked (! owner.editorDocked);   // a click elsewhere opens / closes it
            }

            repaint();

            owner.engine.getSettingsFile().setValue ("dockHeight", owner.dockHeight);
            owner.engine.getSettingsFile().saveIfNeeded();
        }

        MainComponent& owner;
        int startHeight = 0;
        bool dragged = false;
    } dockHandle { *this };

    int contentAreaHeight = 0;             // the arrangement + docked editor area (set in resized)
    int maxDockHeight() const              { return contentAreaHeight - dockHandleHeight; }   // up to the timeline bar (the arrangement hidden)
    int currentDockHeight() const
    {
        return juce::jlimit (80, juce::jmax (100, maxDockHeight()), dockHeight > 0 ? dockHeight : contentAreaHeight * 2 / 5);
    }

    // Dragging the side pane's left edge resizes it
    struct SidePaneResizer final : juce::Component
    {
        explicit SidePaneResizer (MainComponent& ownerToUse) : owner (ownerToUse)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        }

        void mouseDown (const juce::MouseEvent&) override { startWidth = owner.currentSidePaneWidth(); }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            owner.sidePaneWidth = juce::jlimit (280, juce::jmax (300, owner.getWidth() / 2), startWidth - event.getDistanceFromDragStartX());
            owner.resized();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            owner.engine.getSettingsFile().setValue ("sidePaneWidth", owner.sidePaneWidth);
            owner.engine.getSettingsFile().saveIfNeeded();
        }

        MainComponent& owner;
        int startWidth = 0;
    } sidePaneResizer { *this };

    int currentSidePaneWidth() const
    {
        return sidePaneWidth > 0 ? juce::jlimit (280, juce::jmax (300, getWidth() / 2), sidePaneWidth)
                                 : juce::jlimit (340, 620, getWidth() * 32 / 100);
    }

    // Sidebar. The track list and the arrangement share one vertical scroll
    // (same Y axis); declared before both.
    sidebar::VerticalScroll trackScroll;
    TrackList trackList { engine, trackScroll };
    int sidebarWidth = 0;            // 0 = not yet computed (defaults to ~15% of the window)

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
            owner.engine.setViewValue ("sidebarWidth", owner.sidebarWidth);   // kept with the project
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
    MixerView mixerView { engine };
    ArrangementView arrangementView { engine, commandDispatcher, timeAxis, trackScroll };
    PianoRollView pianoRollView { engine, commandDispatcher, timeAxis };
    InstrumentsView instrumentsView { engine };
    InstrumentEditorView instrumentEditorView { engine, commandDispatcher };
    ExpressionMapEditorView expressionMapView { engine, commandDispatcher };
    HistoryView historyView { commandDispatcher };
    SettingsView settingsView { engine };

    ContentView contentView = ContentView::midiRegions;
    bool settingsOpen = false;

    // Bottom
    juce::Label statusLabel;
    PerformanceTracker perfTracker { engine };
    PerformancePanel perfPanel { perfTracker };

    //==============================================================================
    AudioEngine::TrackId selectedTrack = 0;
    int lastEngineRevision = -1;    // topbar widgets follow engine mutations
    std::map<AudioEngine::InstrumentId, std::unique_ptr<PluginWindow>> pluginWindows;
    std::map<std::pair<AudioEngine::AudioChannelId, int>, std::unique_ptr<PluginWindow>> insertWindows;   // (channel, slot)

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
