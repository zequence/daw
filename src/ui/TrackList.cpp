#include "TrackList.h"
#include "ColorPalette.h"
#include "ThemedLookAndFeel.h"
#include "../engine/AudioChannelProcessor.h"
#include "MixerParts.h"
#include "ControllerLanes.h"

namespace
{
    using sidebar::indentPerLevel;
}

//==============================================================================
class TrackList::Row final : public juce::Component
{
public:
    Row (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::TrackId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), trackId (id), depth (depthToUse)
    {
        nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);
        nameLabel.setEditable (false, true);
        nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        nameLabel.setFont (sidebar::trackNameFont());
        nameLabel.setColour (juce::Label::textColourId, sidebar::rowTextColour);
        nameLabel.onTextChange = [this]
        {
            engine.setTrackName (trackId, nameLabel.getText());
            nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);
        };
        nameLabel.setInterceptsMouseClicks (false, false);   // single click selects; double click edits

        armButton.setTooltip ("Arm for live input and recording");
        theme::setButtonRole (armButton, "arm");
        armButton.onClick = [this] { if (owner.onArm) owner.onArm (trackId); };

        soloButton.setTooltip ("Solo (MIDI)");
        soloButton.setClickingTogglesState (true);
        theme::setButtonRole (soloButton, "solo");
        soloButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);   // meter|S|M: one unit
        soloButton.onClick = [this] { engine.setTrackSoloed (trackId, soloButton.getToggleState()); };

        muteButton.setTooltip ("Mute (MIDI)");
        muteButton.setClickingTogglesState (true);
        theme::setButtonRole (muteButton, "mute");
        muteButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        muteButton.onClick = [this] { engine.setTrackMuted (trackId, muteButton.getToggleState()); };

        meter.colourFor = [] (float v) { return lanes::valueColour (lanes::Kind::velocity, v); };
        meter.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (meter);

        for (auto* c : std::initializer_list<juce::Component*> { &armButton, &soloButton, &muteButton })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }

        addAndMakeVisible (nameLabel);
    }

    AudioEngine::TrackId getTrackId() const noexcept { return trackId; }

    void refresh (bool isSelected, bool isArmed, bool isSubselected = false)
    {
        selected = isSelected;
        subselected = isSubselected;
        meter.update (engine.takeTrackMidiActivity (trackId));
        armButton.setToggleState (isArmed, juce::dontSendNotification);
        muteButton.setToggleState (engine.isTrackMuted (trackId), juce::dontSendNotification);
        soloButton.setToggleState (engine.isTrackSoloed (trackId), juce::dontSendNotification);

        if (! nameLabel.isBeingEdited())
            nameLabel.setText (engine.getTrackName (trackId), juce::dontSendNotification);

        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            if (owner.onShowContextMenu)
                owner.onShowContextMenu (trackId);
            return;
        }

        owner.rowMouseDown (this, false, trackId, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, false, trackId, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.finishRowDrag (trackId);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);

        theme::paintTrackBox (g, bounds, theme::Token::trackMidiBg, selected, subselected);

        if (engine.getTrackInstrument (trackId) == 0)   // outside an instrument: its colour as a left border (grey without)
        {
            g.setColour (AudioEngine::colourFromHex (engine.getTrackColour (trackId), juce::Colour (0xff6d7178)));
            g.fillRect (bounds.getX() + 1.0f, bounds.getY() + 1.0f, 8.0f, bounds.getHeight() - 2.0f);
        }

        sidebar::drawTrackIcon (g, iconBox.toFloat(), sidebar::TrackKind::midi, sidebar::rowTextColour.withAlpha (0.8f));

        // Shown in the MIDI editor: a bar on the right edge - wider and brighter for the edited one
        const auto& shown = owner.editorTracks;

        if (std::find (shown.begin(), shown.end(), trackId) != shown.end())
        {
            const auto edited = owner.editorTrack == trackId;
            const auto width = edited ? 5.0f : 2.5f;
            g.setColour (theme::colour (theme::Token::channelEdited).withAlpha (edited ? 1.0f : 0.55f));
            g.fillRect (bounds.getRight() - width - 1.0f, bounds.getY() + 1.0f, width, bounds.getHeight() - 2.0f);
        }
    }

    void resized() override
    {
        // One line: its MIDI meter, S, M (one unit, as the audio rows), the name, and R at the right edge
        auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 6).reduced (8, 0);   // past the colour strip
        area = area.withSizeKeepingCentre (area.getWidth(), 20);

        area.removeFromRight (5);   // the editor bar's room
        armButton.setBounds (area.removeFromRight (20));
        area.removeFromRight (6);
        iconBox = area.removeFromLeft (sidebar::iconWidth);   // its kind: the MIDI plug
        area.removeFromLeft (5);
        meter.setBounds (area.removeFromLeft (7));
        area.removeFromLeft (1);
        soloButton.setBounds (area.removeFromLeft (20));
        muteButton.setBounds (area.removeFromLeft (20).expanded (1, 0).withTrimmedRight (1));   // shares S's right edge
        area.removeFromLeft (6);
        nameLabel.setBounds (area.translated (0, sidebar::visualCentreOffset (nameLabel.getFont())));
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::TrackId trackId;
    const int depth;

    juce::Label nameLabel;
    juce::TextButton armButton { "R" }, soloButton { "S" }, muteButton { "M" };
    mixer::MidiMeter meter;
    juce::Rectangle<int> iconBox;
    bool selected = false, subselected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
};

//==============================================================================
class TrackList::FolderRow final : public juce::Component
{
public:
    FolderRow (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::FolderId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), folderId (id), depth (depthToUse)
    {
        nameLabel.setText (engine.getFolderName (folderId).toUpperCase(), juce::dontSendNotification);
        nameLabel.setEditable (false, true);
        nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        // Same text color as the tracks; the bold smaller font sets folders apart (ISSUES.md)
        nameLabel.setFont (sidebar::folderNameFont());
        nameLabel.onEditorShow = [this]   // shown in capitals; renamed as written
        {
            if (auto* editor = nameLabel.getCurrentTextEditor())
                editor->setText (engine.getFolderName (folderId), false);
        };
        nameLabel.setColour (juce::Label::textColourId, sidebar::rowTextColour);   // the same white as the others
        nameLabel.onTextChange = [this]
        {
            engine.setFolderName (folderId, nameLabel.getText());
            nameLabel.setText (engine.getFolderName (folderId).toUpperCase(), juce::dontSendNotification);
        };
        nameLabel.setInterceptsMouseClicks (false, false);   // single click toggles; double click edits
        addAndMakeVisible (nameLabel);
    }

    void refresh()
    {
        if (! nameLabel.isBeingEdited())
            nameLabel.setText (engine.getFolderName (folderId).toUpperCase(), juce::dontSendNotification);

        repaint();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            owner.showFolderMenu (folderId);
            return;
        }

        owner.rowMouseDown (this, true, folderId, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, true, folderId, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        // A click (no drag happened): the arrow toggles collapse; the rest of
        // the row selects the folder (ISSUES.md) - which selects every track
        // inside it, ready for multi-channel work.
        if (! owner.finishRowDrag (folderId))
        {
            if (event.x < iconBox.getRight() + 2)   // the arrow or the folder symbol: open / close
                engine.setFolderCollapsed (folderId, ! engine.isFolderCollapsed (folderId));
            else
                owner.selectFolder (folderId);
        }
        // finishRowDrag already scheduled the refresh (deferred: it may delete this row)
    }

    void setSelected (bool shouldBeSelected, bool shouldBeSubselected)
    {
        if (selected != shouldBeSelected || subselected != shouldBeSubselected)
        {
            selected = shouldBeSelected;
            subselected = shouldBeSubselected;
            repaint();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameLabel.getBounds().contains (event.getPosition()))
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
        theme::paintTrackBox (g, bounds, theme::Token::folderBg, selected, subselected);

        // Collapse triangle (folders have no colour: it sits near the edge)
        const auto collapsed = engine.isFolderCollapsed (folderId);
        juce::Path triangle;
        const auto cx = bounds.getX() + 9.0f, cy = bounds.getCentreY();

        if (collapsed)
            triangle.addTriangle (cx - 1.5f, cy - 3.0f, cx - 1.5f, cy + 3.0f, cx + 3.0f, cy);
        else
            triangle.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 3.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillPath (triangle);
        sidebar::drawTrackIcon (g, iconBox.toFloat(), sidebar::TrackKind::folder, sidebar::rowTextColour.withAlpha (0.8f));
    }

    void resized() override
    {
        // The arrow, the folder symbol, then the name (as an instrument's: arrow | 6 | symbol | 5 | name)
        iconBox = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20).withWidth (sidebar::iconWidth);
        nameLabel.setBounds (getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20 + sidebar::iconWidth + 5).reduced (0, 2)
                                 .translated (0, sidebar::visualCentreOffset (nameLabel.getFont())));
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::FolderId folderId;
    const int depth;
    juce::Label nameLabel;
    juce::Rectangle<int> iconBox;
    bool selected = false, subselected = false;

public:
    AudioEngine::FolderId getFolderId() const noexcept { return folderId; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FolderRow)
};

//==============================================================================
TrackList::TrackList (AudioEngine& e, sidebar::VerticalScroll& v) : engine (e), vscroll (v), wheelZoom { *this }
{
    // Adding tracks/folders lives in the right-click menus (ISSUES.md: header
    // buttons removed)
    viewport.setViewedComponent (&rowContainer, false);
    viewport.setScrollBarsShown (false, false, true, false);   // no scrollbar; the wheel still scrolls (ISSUES.md)
    addAndMakeVisible (viewport);
    viewport.addMouseListener (&wheelZoom, true);   // Ctrl+Shift+wheel anywhere in the list: track height
}

//==============================================================================
// An instrument folder: its tracks (those whose first output it is) and then its audio. Like a folder
// row - the arrow opens and closes it, a click selects it (the editor then takes all its tracks),
// a drag moves all its tracks
class TrackList::InstrumentRow final : public juce::Component
{
public:
    InstrumentRow (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::InstrumentId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), instrumentId (id), depth (depthToUse)
    {
        // Solo and mute for the whole instrument (its audio), and its level
        soloButton.setTooltip ("Solo the instrument");
        soloButton.setClickingTogglesState (true);
        theme::setButtonRole (soloButton, "solo");
        soloButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);   // meter|S|M: one unit
        soloButton.onClick = [this]
        {
            if (const auto channel = engine.getAudioChannelForInstrument (instrumentId); channel != 0)
                engine.setAudioChannelSoloed (channel, soloButton.getToggleState());
        };

        muteButton.setTooltip ("Mute the instrument");
        muteButton.setClickingTogglesState (true);
        theme::setButtonRole (muteButton, "mute");
        muteButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        muteButton.onClick = [this]
        {
            if (auto* p = engine.getAudioChannel (engine.getAudioChannelForInstrument (instrumentId)))
                p->setMuted (muteButton.getToggleState());
        };

        for (auto* c : std::initializer_list<juce::Component*> { &soloButton, &muteButton, &meter })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }

        nameLabel.setFont (sidebar::trackNameFont().withHeight (15.0f));
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::transparentBlack);
        nameLabel.setColour (juce::Label::textWhenEditingColourId, sidebar::rowTextColour);
        nameLabel.setInterceptsMouseClicks (false, true);   // the row gets the clicks; the editor its own
        nameLabel.onTextChange = [this]
        {
            if (nameLabel.getText().trim().isNotEmpty())
                engine.setInstrumentName (instrumentId, nameLabel.getText().trim());

            repaint();
        };
        addAndMakeVisible (nameLabel);
    }

    void refresh()
    {
        const auto channel = engine.getAudioChannelForInstrument (instrumentId);

        if (auto* p = engine.getAudioChannel (channel))
        {
            muteButton.setToggleState (p->isMuted(), juce::dontSendNotification);
            meter.update (p->getLastPeak());
        }

        soloButton.setToggleState (channel != 0 && engine.isAudioChannelSoloed (channel), juce::dontSendNotification);
    }

    void resized() override
    {
        // The arrow furthest left, then its kind (a keyboard), one unit - its level (a vertical meter as
        // tall as the buttons), S, M - and the tape: arrow | 6 | keyboard | 5 | meter S M | 6 | tape
        auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20).reduced (0, 4);
        iconBox = area.removeFromLeft (sidebar::iconWidth);
        area.removeFromLeft (5);
        meter.setBounds (area.removeFromLeft (7).withSizeKeepingCentre (7, 20));
        area.removeFromLeft (1);
        soloButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20));
        muteButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20).expanded (1, 0).withTrimmedRight (1));
        tapeLeft = area.getX() + 6;
    }

    AudioEngine::InstrumentId getInstrumentId() const noexcept { return instrumentId; }

    void setSelected (bool shouldBeSelected, bool shouldBeSubselected)
    {
        if (selected != shouldBeSelected || subselected != shouldBeSubselected)
        {
            selected = shouldBeSelected;
            subselected = shouldBeSubselected;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            if (owner.onInstrumentMenu)
                owner.onInstrumentMenu (instrumentId);

            return;
        }

        owner.drag = {};
        owner.clearSelectionOnMouseUp = false;
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        if (! owner.drag.active && getLocalBounds().contains (event.getPosition()))
            return;

        if (! owner.drag.active)   // the whole instrument: all its tracks together
        {
            owner.drag.active = true;
            owner.drag.sourceIsFolder = false;
            owner.drag.sourceId = 0;
            owner.drag.draggedTracks = engine.getInstrumentTracks (instrumentId);
        }

        owner.computeDropTarget (event.getEventRelativeTo (&owner.rowContainer).getPosition().y);
        owner.rowContainer.repaint();
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        if (! owner.finishRowDrag (0))
        {
            if (event.x < iconBox.getRight() + 2)   // the arrow or the keyboard symbol: open / close
                engine.setInstrumentExpanded (instrumentId, ! engine.isInstrumentExpanded (instrumentId));
            else
                owner.selectInstrument (instrumentId);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override   // on the name: rename the instrument
    {
        if (event.x >= tapeLeft)
            startRenaming();
    }

    // The name, edited in place over the tape - a label's editor, as a track's name (Return or
    // clicking anywhere else renames, Esc cancels)
    void startRenaming()
    {
        nameLabel.setText (engine.getInstrumentName (instrumentId), juce::dontSendNotification);
        nameLabel.setBounds (getLocalBounds().withLeft (tapeLeft).withRight (getWidth() - 8).withSizeKeepingCentre (getWidth() - 8 - tapeLeft, 22));
        nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
        theme::paintTrackBox (g, bounds, theme::Token::folderBg, selected, subselected);

        const auto tracks = engine.getInstrumentTracks (instrumentId);

        juce::Path triangle;
        const auto cx = bounds.getX() + 9.0f, cy = bounds.getCentreY();   // (as on a folder)

        if (! engine.isInstrumentExpanded (instrumentId))
            triangle.addTriangle (cx - 1.5f, cy - 3.0f, cx - 1.5f, cy + 3.0f, cx + 3.0f, cy);
        else
            triangle.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 3.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillPath (triangle);
        sidebar::drawTrackIcon (g, iconBox.toFloat(), sidebar::TrackKind::instrument, sidebar::rowTextColour.withAlpha (0.8f));

        auto text = getLocalBounds().withLeft (tapeLeft).reduced (0, 2);   // the tape, after the level and S|M

        // Its name on tape - the tape takes the instrument's colour (cream without one); drawn once, cached
        const auto colour = AudioEngine::colourFromHex (engine.getInstrumentColour (instrumentId), mixer::tape::cream);
        nameTape.draw (g, text.withTrimmedLeft (2), engine.getInstrumentName (instrumentId), colour,
                       juce::jmin (64.0f, (float) getHeight() * 0.8f));   // the writing fills the tape (and grows with the zoom)
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::InstrumentId instrumentId;
    const int depth;
    bool selected = false, subselected = false;
    mixer::tape::Cached nameTape;
    juce::TextButton soloButton { "S" }, muteButton { "M" };
    mixer::LevelMeter meter { false };
    juce::Label nameLabel;   // only its editor shows (renaming); the tape draws the name
    int tapeLeft = 0;
    juce::Rectangle<int> iconBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentRow)
};

//==============================================================================
// An instrument's audio, inside its folder after its tracks: name, mute, level and a meter
class TrackList::AudioRow final : public juce::Component
{
public:
    AudioRow (TrackList& ownerToUse, AudioEngine& engineToUse, AudioEngine::AudioChannelId id, int depthToUse)
        : owner (ownerToUse), engine (engineToUse), channelId (id), depth (depthToUse)
    {
        muteButton.setTooltip ("Mute (audio)");
        muteButton.setClickingTogglesState (true);
        theme::setButtonRole (muteButton, "mute");
        muteButton.setWantsKeyboardFocus (false);
        muteButton.onClick = [this]
        {
            if (auto* p = engine.getAudioChannel (channelId))
                p->setMuted (muteButton.getToggleState());
        };
        addAndMakeVisible (muteButton);

        soloButton.setTooltip ("Solo (audio)");
        soloButton.setClickingTogglesState (true);
        theme::setButtonRole (soloButton, "solo");
        soloButton.setWantsKeyboardFocus (false);
        soloButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);   // meter|S|M: one unit
        muteButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        soloButton.onClick = [this] { engine.setAudioChannelSoloed (channelId, soloButton.getToggleState()); };
        addAndMakeVisible (soloButton);
        addAndMakeVisible (meter);

        // Renaming (double-click the name): a label's editor, as a track's; the row draws the name
        nameLabel.setFont (sidebar::trackNameFont());
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::transparentBlack);
        nameLabel.setColour (juce::Label::textWhenEditingColourId, sidebar::rowTextColour);
        nameLabel.setInterceptsMouseClicks (false, true);
        nameLabel.onTextChange = [this]
        {
            if (nameLabel.getText().trim().isNotEmpty())
                engine.setAudioChannelName (channelId, nameLabel.getText().trim());

            repaint();
        };
        addAndMakeVisible (nameLabel);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (nameArea.contains (event.getPosition()))
        {
            nameLabel.setText (engine.getAudioChannelName (channelId), juce::dontSendNotification);
            nameLabel.showEditor();
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
        {
            owner.selectChannel (channelId, event.mods);
            return;
        }

        if (owner.onInstrumentMenu)
            if (const auto instrument = engine.getAudioChannelInput (channelId); instrument != 0)
                owner.onInstrumentMenu (instrument);
    }

    AudioEngine::AudioChannelId getChannelId() const noexcept   { return channelId; }

    void setSelected (bool should)   { if (selected != should) { selected = should; repaint(); } }

    void refresh()
    {
        if (auto* p = engine.getAudioChannel (channelId))
        {
            muteButton.setToggleState (p->isMuted(), juce::dontSendNotification);
            meter.update (p->getLastPeak());
        }

        soloButton.setToggleState (engine.isAudioChannelSoloed (channelId), juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);

        theme::paintTrackBox (g, bounds, theme::Token::trackAudioBg, selected, subselected);

        sidebar::drawTrackIcon (g, iconBox.toFloat(), sidebar::TrackKind::audio, sidebar::rowTextColour.withAlpha (0.8f));

        const auto font = sidebar::trackNameFont();   // as the MIDI tracks
        g.setColour (sidebar::rowTextColour);
        g.setFont (font);
        g.drawText (engine.getAudioChannelName (channelId), nameArea.translated (0, sidebar::visualCentreOffset (font)),
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        // Its kind (a waveform), one unit - its level (a vertical meter as tall as the buttons), S, M - the name
        auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 6).reduced (8, 4);
        iconBox = area.removeFromLeft (sidebar::iconWidth);
        area.removeFromLeft (5);
        meter.setBounds (area.removeFromLeft (7).withSizeKeepingCentre (7, 20));
        area.removeFromLeft (1);
        soloButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20));
        muteButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20).expanded (1, 0).withTrimmedRight (1));
        area.removeFromLeft (8);
        nameArea = area;
        nameLabel.setBounds (nameArea.withSizeKeepingCentre (nameArea.getWidth(), 22));
    }

private:
    TrackList& owner;
    AudioEngine& engine;
    const AudioEngine::AudioChannelId channelId;
    const int depth;
    juce::TextButton soloButton { "S" }, muteButton { "M" };
    mixer::LevelMeter meter { false };
    juce::Rectangle<int> nameArea, iconBox;
    juce::Label nameLabel;   // only its editor shows (renaming)
    bool subselected = false, selected = false;

public:
    void setSubselected (bool should)   { if (subselected != should) { subselected = should; repaint(); } }

private:

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRow)
};

//==============================================================================
TrackList::~TrackList() = default;

void TrackList::setSelectedTrack (AudioEngine::TrackId id)
{
    selectedTrack = id;

    if (id != 0)
    {
        selectedFolder = 0;   // a track chosen (e.g. a folder opened in the editor: its first track) ends the folder selection
        selectedInstrument = 0;
        selectedChannels.clear();
    }

    refresh();
}

void TrackList::refresh()
{
    auto freshItems = engine.getSidebarItems (true, true);

    if (freshItems != items)
    {
        items = std::move (freshItems);
        rebuildRows();
    }

    // Two-way sync with the shared vertical scroll (the arrangement is on the
    // same Y axis): follow it when someone else moved it, push our own scrolling.
    if (vscroll.revision != lastScrollRevision)
        viewport.setViewPosition (viewport.getViewPositionX(), vscroll.y);
    else if (viewport.getViewPositionY() != vscroll.y)
        vscroll.set (viewport.getViewPositionY());

    vscroll.set (viewport.getViewPositionY());   // viewport clamping wins
    lastScrollRevision = vscroll.revision;

    realizeVisibleRows();   // scrolling brings new rows into existence

    // Drop selections that no longer exist
    if (! multiSelection.empty())
    {
        const auto trackIds = engine.getTrackIds();
        const std::set<AudioEngine::TrackId> existing (trackIds.begin(), trackIds.end());
        std::erase_if (multiSelection, [&existing] (auto id) { return existing.count (id) == 0; });
    }

    // A selected folder or instrument selects everything inside it: those rows are subselected (the
    // theme's own colour); the tracks' own selection highlight steps aside meanwhile
    std::set<RowKey> subselectedRows;
    const auto groupSelected = selectedFolder != 0 || selectedInstrument != 0;

    if (groupSelected)
    {
        int groupDepth = -1;

        for (auto& item : items)
        {
            if (groupDepth < 0)
            {
                if ((selectedFolder != 0 && item.folder == selectedFolder) || (selectedInstrument != 0 && item.instrument == selectedInstrument))
                    groupDepth = item.depth;

                continue;
            }

            if (item.depth <= groupDepth)
                break;

            subselectedRows.insert ({ item.folder, item.member, item.depth, item.instrument, item.channel });
        }
    }

    // Only live (near-visible) rows refresh
    for (auto& [key, component] : liveRows)
    {
        const auto sub = subselectedRows.count (key) > 0;

        if (auto* trackRow = dynamic_cast<Row*> (component.get()))
            trackRow->refresh (! groupSelected && selectedChannels.empty()   // (audio rows chosen: no track shows selected)
                                 && (multiSelection.empty() ? trackRow->getTrackId() == selectedTrack
                                                            : multiSelection.count (trackRow->getTrackId()) > 0),
                               engine.isTrackArmed (trackRow->getTrackId()), sub);
        else if (auto* folderRow = dynamic_cast<FolderRow*> (component.get()))
        {
            folderRow->setSelected (folderRow->getFolderId() == selectedFolder, sub);
            folderRow->refresh();
        }
        else if (auto* instrumentRow = dynamic_cast<InstrumentRow*> (component.get()))
        {
            instrumentRow->setSelected (instrumentRow->getInstrumentId() == selectedInstrument, sub);
            instrumentRow->refresh();
        }
        else if (auto* audioRow = dynamic_cast<AudioRow*> (component.get()))
        {
            audioRow->setSubselected (sub);
            audioRow->setSelected (selectedChannels.count (audioRow->getChannelId()) > 0);
            audioRow->refresh();
        }
    }
}

// An instrument folder selected: its first track becomes the selected track (the mixer and the
// editor follow), and the editor (E / D) takes all its tracks
void TrackList::selectInstrument (AudioEngine::InstrumentId instrumentId)
{
    multiSelection.clear();
    selectedFolder = 0;
    selectedChannels.clear();

    if (const auto tracks = engine.getInstrumentTracks (instrumentId); ! tracks.empty() && onSelect)
        onSelect (tracks.front());

    selectedInstrument = instrumentId;   // (after onSelect: choosing the track clears it)

    if (onGroupSelected)
        onGroupSelected();

    refresh();
}

void TrackList::setEditedTracks (std::vector<AudioEngine::TrackId> shown, AudioEngine::TrackId edited)
{
    if (shown == editorTracks && edited == editorTrack)
        return;

    editorTracks = std::move (shown);
    editorTrack = edited;

    for (auto& [key, component] : liveRows)
        component->repaint();
}

// The selection: one thing at a time - a track (or several, Ctrl / Shift), a folder, an instrument
// folder, or an audio row. A folder or an instrument selects everything inside it (subselected);
// choosing any one of them clears everything else, the arrangement's regions too.
void TrackList::selectFolder (AudioEngine::FolderId folderId)
{
    multiSelection.clear();
    selectedInstrument = 0;
    selectedChannels.clear();
    selectedFolder = folderId;

    if (onSelectionChanged)
        onSelectionChanged (multiSelection);

    if (onGroupSelected)
        onGroupSelected();

    refreshSoon();
}

void TrackList::selectChannel (AudioEngine::AudioChannelId channelId, juce::ModifierKeys mods)
{
    auto chosen = selectedChannels;

    if (mods.isCtrlDown())
    {
        if (! chosen.erase (channelId))
            chosen.insert (channelId);
    }
    else if (mods.isShiftDown() && channelAnchor != 0)   // the audio rows from the anchor to here
    {
        chosen.clear();
        bool inRange = false;

        for (auto& item : engine.getSidebarItems (true, true))
        {
            if (item.channel == 0)
                continue;

            const auto edge = item.channel == channelAnchor || item.channel == channelId;

            if (edge || inRange)
                chosen.insert (item.channel);

            if (edge)
            {
                if (inRange || channelAnchor == channelId)
                    break;

                inRange = true;
            }
        }
    }
    else
    {
        chosen = { channelId };
    }

    if (! mods.isShiftDown())
        channelAnchor = channelId;

    selectChannels (chosen);
}

void TrackList::selectChannels (const std::set<AudioEngine::AudioChannelId>& channels)
{
    multiSelection.clear();
    selectedFolder = 0;
    selectedInstrument = 0;
    selectedChannels = channels;

    if (onSelectionChanged)
        onSelectionChanged (multiSelection);

    if (onGroupSelected)
        onGroupSelected();

    refreshSoon();
}

void TrackList::renameInstrument (AudioEngine::InstrumentId instrumentId)
{
    for (auto& [key, component] : liveRows)
        if (auto* row = dynamic_cast<InstrumentRow*> (component.get()); row != nullptr && row->getInstrumentId() == instrumentId)
            row->startRenaming();
}

void TrackList::setSubtreeCollapsed (AudioEngine::FolderId folderId, bool collapsed)
{
    // The folder and every folder below it
    const auto all = engine.getSidebarItems (true, false);
    int folderDepth = -1;

    for (auto& item : all)
    {
        if (folderDepth < 0)
        {
            if (item.folder == folderId)
            {
                folderDepth = item.depth;
                engine.setFolderCollapsed (folderId, collapsed);
            }

            continue;
        }

        if (item.depth <= folderDepth)
            break;   // left the subtree

        if (item.folder != 0)
            engine.setFolderCollapsed (item.folder, collapsed);
    }

    refreshSoon();
}

void TrackList::WheelZoom::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCtrlDown() && event.mods.isShiftDown() && owner.onTrackHeightZoom)   // track height
        owner.onTrackHeightZoom (wheel.deltaY > 0 ? 1 : -1);
}

int TrackList::heightOfItem (const AudioEngine::SidebarItem& item)
{
    return sidebar::heightOf (item);
}

// The list is VIRTUALIZED: only rows near the visible area own components
// (big projects have 1000+ tracks; building them all made collapse/expand
// and every UI tick slow). Rows are keyed by (folder, member, depth), so a
// row survives as long as its item does and scrolling only creates the rows
// that come into view.
void TrackList::rebuildRows()
{
    rowTops.clear();
    int y = 0;

    for (auto& item : items)
    {
        rowTops.push_back (y);
        y += heightOfItem (item);
    }

    totalHeight = y;
    layoutRows();
}

void TrackList::layoutRows()
{
    // At least viewport height, so right-clicking the empty area reaches the container
    rowContainer.setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()),
                          juce::jmax (1, totalHeight, viewport.getHeight()));
    realizeVisibleRows();
}

void TrackList::realizeVisibleRows()
{
    const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
    const auto margin = juce::jmax (200, viewport.getHeight());   // a screen of slack each way
    const auto top = viewport.getViewPositionY() - margin;
    const auto bottom = viewport.getViewPositionY() + viewport.getHeight() + margin;

    std::set<RowKey> wanted;

    for (size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const auto height = heightOfItem (item);

        if (rowTops[i] + height < top || rowTops[i] > bottom)
            continue;

        const RowKey key { item.folder, item.member, item.depth, item.instrument, item.channel };
        wanted.insert (key);

        auto& row = liveRows[key];

        if (row == nullptr)
        {
            if (item.folder != 0)
                row = std::make_unique<FolderRow> (*this, engine, item.folder, item.depth);
            else if (item.instrument != 0)
                row = std::make_unique<InstrumentRow> (*this, engine, item.instrument, item.depth);
            else if (item.channel != 0)
                row = std::make_unique<AudioRow> (*this, engine, item.channel, item.depth);
            else
                row = std::make_unique<Row> (*this, engine, item.member, item.depth);

            rowContainer.addAndMakeVisible (*row);
        }

        row->setBounds (0, rowTops[i], width, height);
    }

    // Retire rows that left the window - but never while a mouse button is down:
    // the row under the cursor may be mid-gesture (drag/click).
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;

    for (auto it = liveRows.begin(); it != liveRows.end();)
    {
        if (wanted.count (it->first) == 0)
            it = liveRows.erase (it);
        else
            ++it;
    }
}

//==============================================================================
// Selection + drag

void TrackList::rowMouseDown (juce::Component*, bool isFolder, int id, const juce::MouseEvent& event)
{
    drag = {};
    clearSelectionOnMouseUp = false;

    if (isFolder)
        return;   // folders don't join the multi-selection; a plain drag moves just the folder

    selectedFolder = 0;   // clicking a track ends a folder selection
    const auto trackId = (AudioEngine::TrackId) id;

    if (event.mods.isCtrlDown())
    {
        // The current single selection becomes the first member of the group
        if (multiSelection.empty() && selectedTrack != 0)
            multiSelection.insert (selectedTrack);

        if (multiSelection.count (trackId))
            multiSelection.erase (trackId);
        else
            multiSelection.insert (trackId);

        shiftAnchor = trackId;
    }
    else if (event.mods.isShiftDown() && shiftAnchor != 0)
    {
        // Range over the visual order of track rows, from the anchor to the click
        multiSelection.clear();
        bool inRange = false;

        for (auto& item : items)
        {
            if (item.member == 0)
                continue;

            const auto isEdge = item.member == shiftAnchor || item.member == trackId;

            if (isEdge || inRange)
                multiSelection.insert (item.member);

            if (isEdge)
            {
                if (inRange || shiftAnchor == trackId)
                    break;          // closing edge (or a one-row range)

                inRange = true;     // opening edge
            }
        }
    }
    else
    {
        // Clicking an already-selected row must not re-select it (that would steal
        // the group drag); it resolves on mouse-up if no drag happened.
        if (multiSelection.count (trackId))
        {
            clearSelectionOnMouseUp = true;
        }
        else
        {
            multiSelection.clear();

            if (onSelect)
                onSelect (trackId);
        }

        shiftAnchor = trackId;
    }

    // Ctrl/Shift changed the multi-selection: the shell arms it (auto-record)
    if ((event.mods.isCtrlDown() || event.mods.isShiftDown()) && onSelectionChanged)
        onSelectionChanged (multiSelection);

    refresh();
}

void TrackList::rowMouseDrag (juce::Component* row, bool isFolder, int id, const juce::MouseEvent& event)
{
    // "Moving only happens when the mouse moves outside of the channel being dragged"
    if (! drag.active && row->getLocalBounds().contains (event.getPosition()))
        return;

    if (! drag.active)
    {
        drag.active = true;
        drag.sourceIsFolder = isFolder;
        drag.sourceId = id;

        if (! isFolder)
            drag.draggedTracks = multiSelection.count (id) ? selectionInVisualOrder()
                                                           : std::vector<AudioEngine::TrackId> { id };
    }

    computeDropTarget (event.getEventRelativeTo (&rowContainer).getPosition().y);
    rowContainer.repaint();
}

void TrackList::computeDropTarget (int y)
{
    drag.valid = false;
    drag.intoFolder = false;
    drag.indicatorY = -1;
    drag.folderHighlight = {};

    const auto isDragged = [this] (const AudioEngine::SidebarItem& item)
    {
        if (drag.sourceIsFolder)
            return item.folder == drag.sourceId;

        return item.member != 0 && std::find (drag.draggedTracks.begin(), drag.draggedTracks.end(),
                                              item.member) != drag.draggedTracks.end();
    };

    // Count the siblings of 'parent' that appear before item index i (dragged ones
    // don't count: the engine removes them from the list before inserting).
    const auto childIndexAt = [&] (AudioEngine::FolderId parent, size_t i)
    {
        int index = 0;

        for (size_t j = 0; j < i; ++j)   // (instrument folders and their audio rows aren't the folder's children)
            if (items[j].parent == parent && items[j].isTreeChild() && ! isDragged (items[j]))
                ++index;

        return index;
    };

    int rowY = 0;
    size_t i = 0;

    for (; i < items.size(); ++i)
    {
        const auto height = heightOfItem (items[i]);

        if (y < rowY + height)
            break;

        rowY += height;
    }

    if (i >= items.size())
    {
        // Below every row: append at the top level
        drag.valid = true;
        drag.parent = 0;
        drag.index = childIndexAt (0, items.size());
        drag.indicatorY = rowY;
        return;
    }

    const auto& item = items[i];
    const auto height = heightOfItem (item);

    // A folder row's middle drops INTO the folder (its edges insert around it)
    if (item.folder != 0 && y >= rowY + height / 4 && y <= rowY + 3 * height / 4)
    {
        if (drag.sourceIsFolder && item.folder == drag.sourceId)
            return;   // not into itself (subtrees are rejected by the engine on drop)

        drag.valid = true;
        drag.intoFolder = true;
        drag.parent = item.folder;
        drag.index = std::numeric_limits<int>::max();   // append
        drag.folderHighlight = { 0, rowTops[i], rowContainer.getWidth(), height };
        return;
    }

    const auto before = y < rowY + height / 2;
    drag.valid = true;
    drag.parent = item.parent;
    drag.index = childIndexAt (item.parent, i) + (before ? 0 : 1);
    drag.indicatorY = before ? rowY : rowY + height;
}

bool TrackList::finishRowDrag (int id)
{
    const auto wasDragging = drag.active;

    if (drag.active && drag.valid)
    {
        std::vector<AudioEngine::FolderId> folderIds;
        std::vector<int> memberIds;

        if (drag.sourceIsFolder)
            folderIds.push_back (drag.sourceId);
        else
            memberIds.assign (drag.draggedTracks.begin(), drag.draggedTracks.end());

        engine.moveSidebarItems (true, folderIds, memberIds, drag.parent, drag.index);
    }
    else if (! wasDragging && clearSelectionOnMouseUp)
    {
        // The deferred plain click on a selected row: now it becomes the selection
        multiSelection.clear();

        if (onSelect)
            onSelect (id);
    }

    clearSelectionOnMouseUp = false;
    drag = {};
    rowContainer.repaint();
    refreshSoon();   // rebuilding rows would delete the row we're called from
    return wasDragging;
}

void TrackList::refreshSoon()
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TrackList> (this)]
                                     { if (safe != nullptr) safe->refresh(); });
}

std::vector<AudioEngine::TrackId> TrackList::selectionInVisualOrder() const
{
    std::vector<AudioEngine::TrackId> ordered;

    for (auto& item : items)
        if (item.member != 0 && multiSelection.count (item.member))
            ordered.push_back (item.member);

    return ordered;
}

void TrackList::RowContainer::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
        owner.showBackgroundMenu();
}

void TrackList::RowContainer::paintOverChildren (juce::Graphics& g)
{
    auto& dragState = owner.drag;

    if (! dragState.active || ! dragState.valid)
        return;

    g.setColour (juce::Colours::gold.withAlpha (0.9f));

    if (dragState.intoFolder)
        g.drawRoundedRectangle (dragState.folderHighlight.toFloat().reduced (2.0f, 1.5f), theme::corner, 2.0f);
    else if (dragState.indicatorY >= 0)
        g.fillRect (0, juce::jlimit (0, juce::jmax (0, getHeight() - 2), dragState.indicatorY - 1), getWidth(), 2);
}

//==============================================================================
void TrackList::showBackgroundMenu()
{
    const auto safe = juce::Component::SafePointer<TrackList> (this);
    juce::PopupMenu menu;

    menu.addItem ("Add track", [safe] { if (safe != nullptr && safe->onAddTrack) safe->onAddTrack(); });
    menu.addItem ("Add folder", [safe]
    {
        if (safe != nullptr)
        {
            safe->engine.addFolder (true);
            safe->refresh();
        }
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TrackList::showFolderMenu (AudioEngine::FolderId folderId)
{
    const auto safe = juce::Component::SafePointer<TrackList> (this);
    juce::PopupMenu menu;

    menu.addItem ("New track inside", [safe, folderId]
    {
        if (safe != nullptr && safe->onAddTrackInFolder)
            safe->onAddTrackInFolder (folderId);
    });

    menu.addItem ("New subfolder", [safe, folderId]
    {
        if (safe != nullptr)
        {
            safe->engine.addFolder (true, {}, folderId);
            safe->engine.setFolderCollapsed (folderId, false);
            safe->refresh();
        }
    });

    menu.addSeparator();
    menu.addItem ("Collapse all (with subfolders)",
                  [safe, folderId] { if (safe != nullptr) safe->setSubtreeCollapsed (folderId, true); });
    menu.addItem ("Expand all (with subfolders)",
                  [safe, folderId] { if (safe != nullptr) safe->setSubtreeCollapsed (folderId, false); });
    menu.addSeparator();

    juce::PopupMenu moveTo;
    moveTo.addItem ("Top level", true, engine.getFolderParent (folderId) == 0, [safe, folderId]
    {
        if (safe != nullptr) { safe->engine.setFolderParent (folderId, 0); safe->refresh(); }
    });

    for (auto id : engine.getFolderIds (true))
    {
        if (id == folderId)
            continue;

        moveTo.addItem (engine.getFolderName (id), true, engine.getFolderParent (folderId) == id, [safe, folderId, id]
        {
            if (safe != nullptr) { safe->engine.setFolderParent (folderId, id); safe->refresh(); }
        });
    }

    menu.addSubMenu ("Move to folder", moveTo);
    menu.addSeparator();
    menu.addItem ("Remove folder (contents move up)", [safe, folderId]
    {
        if (safe != nullptr) { safe->engine.removeFolder (folderId); safe->refresh(); }
    });

    menu.showMenuAsync (juce::PopupMenu::Options());
}

void TrackList::resized()
{
    viewport.setBounds (getLocalBounds());
    layoutRows();
}

void TrackList::paint (juce::Graphics& g)
{
    g.fillAll (theme::colour (theme::Token::sidebarBg));
}
