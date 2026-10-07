#pragma once

#include "AudioEngine.h"
#include "UserData.h"
#include "PluginWindow.h"
#include "PluginScanProcess.h"
#include "diagnostics/PerformancePanel.h"
#include "ui/TrackList.h"
#include "ui/AudioChannelList.h"
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
//   sidebar (menu, domain buttons, tracks) + content container + side pane / the transport bar
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
    enum class ContentView { midiRegions, midiEditor, audioRegions, instrumentEditor, expressionMaps, mixer };   // Instruments and History: the side pane

    // Instruments and History open as a pane on the right, beside the arrangement or the editor
    enum class SidePane { none, instruments, history };
    SidePane sidePane = SidePane::none;
    SidePane lastSidePane = SidePane::instruments;   // what the expand button opens
    void toggleSidePane (SidePane);
    void zoomTrackHeight (int direction);   // +1 taller, -1 shorter (saved in the settings)
    ContentView mainView = ContentView::midiRegions;   // where the instrument / map editors go back to
    enum class Domain { midi, audio };

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
    void openInsertWindow (AudioEngine::AudioChannelId, int slot);
    void closeInsertWindows (AudioEngine::AudioChannelId, int slot);   // slot -1: all
    void openTrackPluginWindow (AudioEngine::TrackId);   // toggles the track's (first) instrument GUI

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
    void removeInstrumentAsking (AudioEngine::InstrumentId);
    bool triggerArticulation (const std::function<std::optional<ExpressionMap::Target> (const ExpressionMap&)>& find);
    static juce::File startupProjectFile()    { return UserData::getDir().getChildFile ("Startup.odaw"); }
    void openSettings();
    void closeSettings();
    void updateViewVisibility();
    void updatePlaceholders();
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
    juce::TextButton menuButton { "Menu" }, midiDomainButton { "Midi" }, audioDomainButton { "Audio" },
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
    juce::Rectangle<int> paneTabs;         // the side pane's tab row (painted)
    int sidePaneEdge = -1;                 // the side pane's left edge (a line is painted there)
    int sidePaneWidth = 0;                 // 0 = the default (about a third of the window); saved in the settings

    // The MIDI editor docked under the arrangement: a thin handle at the bottom - click to open or
    // close it, drag to set its height - so the arrangement and the editor show together
    bool editorDocked = false;
    int dockHeight = 0;                    // 0 = the default (about 40% of the content); saved in the settings
    static constexpr int dockHandleHeight = 7;
    bool isEditorDockedShowing() const      { return editorDocked && contentView == ContentView::midiRegions; }
    bool isEditorShowing() const            { return contentView == ContentView::midiEditor || isEditorDockedShowing(); }
    void setEditorDocked (bool);

    struct DockHandle final : juce::Component, juce::SettableTooltipClient
    {
        explicit DockHandle (MainComponent& o) : owner (o)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            setTooltip ("MIDI editor: click to open or close it here, drag to set its height");
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (isMouseOver() ? 0xff2c3036 : 0xff24272c));
            const auto grip = juce::Rectangle<float> (36.0f, 3.0f).withCentre (getLocalBounds().toFloat().getCentre());
            g.setColour (juce::Colours::white.withAlpha (owner.editorDocked ? 0.5f : 0.35f));
            g.fillRoundedRectangle (grip, 1.5f);
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

        void mouseUp (const juce::MouseEvent&) override
        {
            if (! dragged)
                owner.setEditorDocked (! owner.editorDocked);   // a click opens / closes it

            owner.engine.getSettingsFile().setValue ("dockHeight", owner.dockHeight);
            owner.engine.getSettingsFile().saveIfNeeded();
        }

        MainComponent& owner;
        int startHeight = 0;
        bool dragged = false;
    } dockHandle { *this };

    int contentAreaHeight = 0;             // the arrangement + docked editor area (set in resized)
    int maxDockHeight() const              { return contentAreaHeight - dockHandleHeight - 60; }
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
    // Above the lists: MIDI / AUDIO in a block of the domain's colour, the letters in
    // the background colour (inverted). At the sidebar's minimum width the space beside the
    // block equals the space above and below it; the letters fill it (both words the same size).
    struct SidebarHeader final : juce::Component
    {
        static constexpr int minSidebarWidth = 150;
        static constexpr int margin = 12;                                  // around the block, every side
        static constexpr int blockWidth = minSidebarWidth - 2 * margin, padding = 16;

        static juce::Font font()
        {
            auto base = juce::Font (juce::FontOptions (12.0f, juce::Font::bold | juce::Font::italic).withKerningFactor (0.12f));
            const auto width = juce::GlyphArrangement::getStringWidth (base, "AUDIO");   // the longer word
            return base.withHeight (12.0f * (float) (blockWidth - 2 * padding) / juce::jmax (1.0f, width));
        }

        // The block fits the strip it's given (as tall as the timeline bar): its letters shrink to
        // the height there is, never wider than at the minimum sidebar width
        juce::Font fittedFont (int blockHeight) const
        {
            const auto widthFit = font();
            const auto heightFit = widthFit.withHeight ((float) blockHeight / 0.95f * 0.8f);
            return heightFit.getHeight() < widthFit.getHeight() ? heightFit : widthFit;
        }

        void set (const juce::String& newText, juce::Colour newColour)
        {
            text = newText;
            colour = newColour;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto blockHeight = juce::jmax (8, getHeight() - 4);   // 2 px above and below
            const auto block = juce::Rectangle<int> (0, 0, juce::jmin (blockWidth, getWidth() - 2 * margin), blockHeight)
                                   .withCentre ({ getWidth() / 2, getHeight() / 2 });

            g.setColour (colour);
            g.fillRoundedRectangle (block.toFloat(), theme::corner);
            g.setColour (theme::colour (theme::Token::surfaceWindow));
            g.setFont (fittedFont (blockHeight));
            g.drawText (text, block, juce::Justification::centred, false);
        }

        juce::String text;
        juce::Colour colour;
    } sidebarHeader;
    TrackList trackList { engine, trackScroll };
    AudioChannelList channelList { engine };
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
    PlaceholderView audioRegionsView { "Audio arrangement" };
    MixerView mixerView { engine };
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
