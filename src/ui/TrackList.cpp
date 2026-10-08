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

        owner.rowMouseDown (this, { RowRef::Kind::track, trackId }, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, { RowRef::Kind::track, trackId }, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.finishRowDrag ({ RowRef::Kind::track, trackId });
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
        setUpGroup();
    }

    void refresh()
    {
        if (! nameLabel.isBeingEdited())
            nameLabel.setText (engine.getFolderName (folderId).toUpperCase(), juce::dontSendNotification);

        refreshGroup();

        // An ungrouped folder labels what it holds: midi, instrument, audio, bus (all of them: mixed)
        const auto contents = engine.getFolderContents (folderId);
        juce::StringArray labels;

        if (contents.midi && contents.instrument && contents.audio && contents.bus)
            labels.add ("mixed");
        else
        {
            if (contents.midi)       labels.add ("midi");
            if (contents.instrument) labels.add ("instrument");
            if (contents.audio)      labels.add ("audio");
            if (contents.bus)        labels.add ("bus");
        }

        if (labels != kindLabels)
        {
            kindLabels = labels;
            resized();
        }

        repaint();
    }

    // The group button (at the right): sums the audio inside the folder on a bus of its own. A grouped
    // folder gets a level meter, S and M (its bus) and its name on tape, as an instrument
    void setUpGroup()
    {
        groupButton.setTooltip ("Group: the audio inside this folder summed on a bus of its own (one strip in the mixer)");
        groupButton.setClickingTogglesState (true);
        groupButton.setWantsKeyboardFocus (false);
        groupButton.onClick = [this] { engine.setFolderGrouped (folderId, groupButton.getToggleState()); owner.refreshSoon(); };
        addAndMakeVisible (groupButton);

        soloButton.setTooltip ("Solo the group");
        soloButton.setClickingTogglesState (true);
        theme::setButtonRole (soloButton, "solo");
        soloButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        soloButton.onClick = [this]   // the channels it sums
        {
            const auto bus = engine.getFolderGroupBus (folderId);

            for (auto channel : engine.getAudioChannelIds())
                if (engine.getAudioChannelOutput (channel) == bus)
                    engine.setAudioChannelSoloed (channel, soloButton.getToggleState());
        };

        muteButton.setTooltip ("Mute the group");
        muteButton.setClickingTogglesState (true);
        theme::setButtonRole (muteButton, "mute");
        muteButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        muteButton.onClick = [this]
        {
            if (auto* p = engine.getAudioChannel (engine.getFolderGroupBus (folderId)))
                p->setMuted (muteButton.getToggleState());
        };

        for (auto* c : std::initializer_list<juce::Component*> { &soloButton, &muteButton, &meter })
        {
            c->setWantsKeyboardFocus (false);
            addChildComponent (c);
        }
    }

    void refreshGroup()
    {
        const auto bus = engine.getFolderGroupBus (folderId);
        const auto grouped = bus != 0;
        groupButton.setToggleState (grouped, juce::dontSendNotification);

        for (auto* c : std::initializer_list<juce::Component*> { &soloButton, &muteButton, &meter })
            c->setVisible (grouped);

        nameLabel.setColour (juce::Label::textColourId, grouped ? juce::Colours::transparentBlack : sidebar::rowTextColour);

        if (auto* p = engine.getAudioChannel (bus))
        {
            meter.update (p->getLastPeak());
            muteButton.setToggleState (p->isMuted(), juce::dontSendNotification);
        }

        bool anySoloed = false;

        for (auto channel : engine.getAudioChannelIds())
            if (grouped && engine.getAudioChannelOutput (channel) == bus)
                anySoloed = anySoloed || engine.isAudioChannelSoloed (channel);

        soloButton.setToggleState (anySoloed, juce::dontSendNotification);

        if (grouped != wasGrouped)
        {
            wasGrouped = grouped;
            resized();
        }
    }

    juce::TextButton groupButton { juce::String::fromUTF8 ("\xce\xa3") }, soloButton { "S" }, muteButton { "M" };
    mixer::LevelMeter meter { false };
    mixer::tape::Cached nameTape;
    bool wasGrouped = false;
    juce::StringArray kindLabels;
    juce::Rectangle<int> kindLabelArea;

    static juce::Font kindLabelFont()   { return juce::Font (juce::FontOptions (12.0f, juce::Font::bold)); }

    static int kindLabelWidth (const juce::String& text)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (kindLabelFont(), text)) + 12;
    }

    static juce::Colour kindLabelColour (const juce::String& text)
    {
        if (text == "midi")       return juce::Colour (0xff5b9bf0);   // blueish
        if (text == "instrument") return juce::Colour (0xff5cc47a);   // greenish
        if (text == "audio")      return juce::Colour (0xffe8655f);   // reddish
        if (text == "bus")        return juce::Colour (0xffc864d8);   // purple / magenta
        return sidebar::rowTextColour;                                 // mixed
    }


    void mouseDown (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            owner.showFolderMenu (folderId);
            return;
        }

        owner.rowMouseDown (this, { RowRef::Kind::folder, folderId }, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, { RowRef::Kind::folder, folderId }, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        // A click (no drag happened): the arrow toggles collapse; the rest of
        // the row selects the folder (ISSUES.md) - which selects every track
        // inside it, ready for multi-channel work.
        if (! owner.finishRowDrag ({ RowRef::Kind::folder, folderId }))
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
        const auto groupColour = AudioEngine::colourFromHex (engine.getFolderColour (folderId), mixer::tape::cream);
        auto marks = juce::Colours::white.withAlpha (0.7f);

        if (wasGrouped)   // a group: the whole row in its colour (its tape dark, written in it)
        {
            g.setColour (selected ? groupColour.brighter (0.2f) : groupColour);
            g.fillRoundedRectangle (bounds, 3.0f);
            marks = mixer::tape::inkFor (groupColour);
        }

        // Collapse triangle (folders have no colour: it sits near the edge)
        const auto collapsed = engine.isFolderCollapsed (folderId);
        juce::Path triangle;
        const auto cx = bounds.getX() + 9.0f, cy = bounds.getCentreY();

        if (collapsed)
            triangle.addTriangle (cx - 1.5f, cy - 3.0f, cx - 1.5f, cy + 3.0f, cx + 3.0f, cy);
        else
            triangle.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 3.0f);

        g.setColour (marks);
        g.fillPath (triangle);
        sidebar::drawTrackIcon (g, iconBox.toFloat(), sidebar::TrackKind::folder, wasGrouped ? marks : sidebar::rowTextColour.withAlpha (0.8f));

        if (wasGrouped && ! nameLabel.isBeingEdited())   // a group: a dark tape written in its colour (as in the mixer)
            nameTape.draw (g, nameLabel.getBounds().withHeight (getHeight()).withY (0), engine.getFolderName (folderId),
                           theme::colour (theme::Token::buttonBg), juce::jmin (48.0f, (float) getHeight() * 0.8f), groupColour);

        if (! wasGrouped)   // the kinds it holds, as small labels
        {
            g.setFont (kindLabelFont());
            auto area = kindLabelArea;

            for (auto& text : kindLabels)
            {
                const auto pill = area.removeFromLeft (kindLabelWidth (text)).withSizeKeepingCentre (kindLabelWidth (text), 17).toFloat();
                area.removeFromLeft (4);

                if (pill.getRight() > (float) kindLabelArea.getRight())
                    break;

                const auto colour = kindLabelColour (text);
                g.setColour (colour.withAlpha (0.22f));
                g.fillRoundedRectangle (pill, 8.5f);
                g.setColour (colour.withAlpha (0.8f));
                g.drawRoundedRectangle (pill.reduced (0.5f), 8.5f, 1.0f);
                g.setColour (colour.brighter (0.6f));
                g.drawText (text, pill, juce::Justification::centred, false);
            }
        }
    }

    void resized() override
    {
        // The arrow, the folder symbol, (grouped: its meter, S, M,) then the name; the group button at the right
        iconBox = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20).withWidth (sidebar::iconWidth);
        auto rest = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20 + sidebar::iconWidth + 5);
        groupButton.setBounds (rest.removeFromRight (28).withSizeKeepingCentre (20, 20));

        if (wasGrouped)
        {
            auto unit = rest.removeFromLeft (48).withSizeKeepingCentre (48, 20);
            meter.setBounds (unit.removeFromLeft (7));
            unit.removeFromLeft (1);
            soloButton.setBounds (unit.removeFromLeft (20));
            muteButton.setBounds (unit.removeFromLeft (20).expanded (1, 0).withTrimmedRight (1));
            rest.removeFromLeft (6);
            kindLabelArea = {};
        }
        else   // the kind labels at the right, before the group button; the name keeps at least half
        {
            int width = 0;

            for (auto& text : kindLabels)
                width += kindLabelWidth (text) + 4;

            width = juce::jmin (width, rest.getWidth() / 2);
            kindLabelArea = rest.removeFromRight (width);
        }

        nameLabel.setBounds (rest.reduced (0, 2)
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
        soloButton.onClick = [this]   // its channel, or the channels its group sums
        {
            if (engine.isSingleOutputInstrument (instrumentId))
            {
                engine.setAudioChannelSoloed (engine.getAudioChannelForInstrument (instrumentId), soloButton.getToggleState());
                return;
            }

            const auto bus = engine.getInstrumentGroupBus (instrumentId);

            for (auto channel : engine.getAudioChannelIds())
                if (engine.getAudioChannelOutput (channel) == bus)
                    engine.setAudioChannelSoloed (channel, soloButton.getToggleState());
        };

        muteButton.setTooltip ("Mute the instrument");
        muteButton.setClickingTogglesState (true);
        theme::setButtonRole (muteButton, "mute");
        muteButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        muteButton.onClick = [this]
        {
            if (auto* p = engine.getAudioChannel (strip()))
                p->setMuted (muteButton.getToggleState());
        };

        for (auto* c : std::initializer_list<juce::Component*> { &soloButton, &muteButton, &meter })
        {
            c->setWantsKeyboardFocus (false);
            addChildComponent (c);   // (a group's)
        }

        groupButton.setTooltip ("Group: the instrument's audio summed on a bus of its own (one strip in the mixer)");
        groupButton.setClickingTogglesState (true);
        groupButton.setWantsKeyboardFocus (false);
        groupButton.onClick = [this] { engine.setInstrumentGrouped (instrumentId, groupButton.getToggleState()); owner.refreshSoon(); };
        addAndMakeVisible (groupButton);

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

    // The strip the row stands for: its one output's channel, or (several outputs) its group's bus
    AudioEngine::AudioChannelId strip() const
    {
        return engine.isSingleOutputInstrument (instrumentId) ? engine.getAudioChannelForInstrument (instrumentId)
                                                               : engine.getInstrumentGroupBus (instrumentId);
    }

    void refresh()
    {
        // Its tag, level, S and M: its one output's (unless a group sums it), or its group's; else folder-like
        const auto bus = strip();
        const auto single = engine.isSingleOutputInstrument (instrumentId);
        const auto grouped = bus != 0 && ! engine.isGroupBus (engine.getAudioChannelOutput (bus));
        groupButton.setVisible (! single);   // (one output: nothing to group)
        groupButton.setToggleState (engine.isInstrumentGrouped (instrumentId), juce::dontSendNotification);

        for (auto* c : std::initializer_list<juce::Component*> { &soloButton, &muteButton, &meter })
            c->setVisible (grouped);

        if (auto* p = engine.getAudioChannel (bus))
        {
            muteButton.setToggleState (p->isMuted(), juce::dontSendNotification);
            meter.update (p->getLastPeak());
        }

        bool anySoloed = false;

        if (single)
            anySoloed = engine.isAudioChannelSoloed (bus);
        else
            for (auto channel : engine.getAudioChannelIds())
                if (grouped && engine.getAudioChannelOutput (channel) == bus)
                    anySoloed = anySoloed || engine.isAudioChannelSoloed (channel);

        soloButton.setToggleState (anySoloed, juce::dontSendNotification);

        if (grouped != wasGrouped)
        {
            wasGrouped = grouped;
            resized();
            repaint();
        }
    }

    void resized() override
    {
        // The arrow furthest left, then its kind (a keyboard), one unit - its level (a vertical meter as
        // tall as the buttons), S, M - and the tape: arrow | 6 | keyboard | 5 | meter S M | 6 | tape
        auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 20).reduced (0, 4);
        iconBox = area.removeFromLeft (sidebar::iconWidth);
        area.removeFromLeft (5);
        groupButton.setBounds (area.removeFromRight (28).withSizeKeepingCentre (20, 20));

        if (wasGrouped)   // a group: its unit, then the tape
        {
            meter.setBounds (area.removeFromLeft (7).withSizeKeepingCentre (7, 20));
            area.removeFromLeft (1);
            soloButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20));
            muteButton.setBounds (area.removeFromLeft (20).withSizeKeepingCentre (20, 20).expanded (1, 0).withTrimmedRight (1));
            area.removeFromLeft (6);
        }

        tapeLeft = area.getX();
        tapeRight = area.getRight();
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

        owner.rowMouseDown (this, { RowRef::Kind::instrument, instrumentId }, event);
    }

    void mouseDrag (const juce::MouseEvent& event) override   // the whole instrument: all its tracks together
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, { RowRef::Kind::instrument, instrumentId }, event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
            return;

        if (! owner.finishRowDrag ({ RowRef::Kind::instrument, instrumentId }))
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
        theme::paintTrackBox (g, bounds, theme::Token::instrumentBg, selected, subselected);

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

        if (nameLabel.isBeingEdited())
            return;

        text = text.withRight (tapeRight);

        if (wasGrouped)   // a group: its name on tape, in the instrument's colour (cream without one); cached
        {
            // (a group - several outputs - dark, written in its colour; one output: its channel's tape)
            const auto colour = AudioEngine::colourFromHex (engine.getInstrumentColour (instrumentId), mixer::tape::cream);
            const auto group = ! engine.isSingleOutputInstrument (instrumentId);
            nameTape.draw (g, text.withTrimmedLeft (2), engine.getInstrumentName (instrumentId),
                           group ? theme::colour (theme::Token::buttonBg) : colour,
                           juce::jmin (64.0f, (float) getHeight() * 0.8f), group ? colour : juce::Colours::transparentBlack);
        }
        else   // as a folder's name
        {
            const auto font = sidebar::folderNameFont();
            g.setColour (sidebar::rowTextColour);
            g.setFont (font);
            g.drawText (engine.getInstrumentName (instrumentId).toUpperCase(), text.translated (0, sidebar::visualCentreOffset (font)),
                        juce::Justification::centredLeft, true);
        }
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
    juce::TextButton groupButton { juce::String::fromUTF8 ("\xce\xa3") };
    bool wasGrouped = false;
    int tapeLeft = 0, tapeRight = 0;
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
            owner.rowMouseDown (this, ref(), event);
            return;
        }

        if (engine.isAudioTrack (channelId))   // an audio track: its own menu
        {
            const auto id = channelId;
            auto& eng = engine;
            juce::PopupMenu menu;
            menu.addSubMenu ("Color", colours::buildMenu (eng.getAudioTrackColour (id),
                                                          [&eng, id, list = juce::Component::SafePointer<TrackList> (&owner)] (juce::String hex)
            {
                eng.setAudioTrackColour (id, hex);

                if (list != nullptr)
                    list->refreshSoon();   // its tag, at once (deferred: this row may be rebuilt)
            }));
            menu.addSeparator();
            menu.addItem ("Remove audio track", [&eng, id] { eng.removeAudioTrack (id); });
            menu.showMenuAsync (juce::PopupMenu::Options());
            return;
        }

        if (owner.onInstrumentMenu)
            if (const auto instrument = engine.getAudioChannelInput (channelId); instrument != 0)
                owner.onInstrumentMenu (instrument);
    }

    AudioEngine::AudioChannelId getChannelId() const noexcept   { return channelId; }

    // An audio track moves (dragged, alone or with others); an instrument's output only selects
    RowRef ref() const   { return { engine.isAudioTrack (channelId) ? RowRef::Kind::audioTrack : RowRef::Kind::channel, channelId }; }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.rowMouseDrag (this, ref(), event);
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            owner.finishRowDrag (ref());
    }

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

        const auto isBus = engine.isBus (channelId);
        theme::paintTrackBox (g, bounds, isBus ? theme::Token::trackBusBg : theme::Token::trackAudioBg, selected, subselected);
        sidebar::drawTrackIcon (g, iconBox.toFloat(), isBus ? sidebar::TrackKind::bus : sidebar::TrackKind::audio,
                                sidebar::rowTextColour.withAlpha (0.8f));

        if (nameLabel.isBeingEdited())
            return;

        if (engine.isGroupBus (engine.getAudioChannelOutput (channelId)))   // summed by a group: no tag, just its name
        {
            const auto font = sidebar::trackNameFont();
            g.setColour (sidebar::rowTextColour);
            g.setFont (font);
            g.drawText (engine.getAudioChannelName (channelId), nameArea.translated (0, sidebar::visualCentreOffset (font)),
                        juce::Justification::centredLeft, true);
            return;
        }

        // Its name on tape (as a group's): its instrument's colour, cream for a bus
        {
            nameTape.draw (g, nameArea.withHeight (getHeight()).withY (0), engine.getAudioChannelName (channelId),
                           AudioEngine::colourFromHex (engine.getChannelTagColour (channelId), mixer::tape::cream),
                           juce::jmin (48.0f, (float) getHeight() * 0.8f));
        }
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
    mixer::tape::Cached nameTape;
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
        selectedFolders.clear();   // a track chosen (e.g. a folder opened in the editor: its first track) ends the folder selection
        selectedInstruments.clear();
        selectedChannels.clear();
    }

    refresh();
}

void TrackList::refresh()
{
    auto freshItems = sidebar::visibleItems (engine);

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
    const auto groupSelected = ! selectedFolders.empty() || ! selectedInstruments.empty();

    if (groupSelected)
    {
        int groupDepth = -1;

        for (auto& item : items)
        {
            if (groupDepth >= 0 && item.depth <= groupDepth)
                groupDepth = -1;

            if (groupDepth >= 0)
                subselectedRows.insert ({ item.folder, item.member, item.depth, item.instrument, item.channel });
            else if (selectedFolders.count (item.folder) > 0 || selectedInstruments.count (item.instrument) > 0)
                groupDepth = item.depth;
        }
    }

    // Only live (near-visible) rows refresh
    for (auto& [key, component] : liveRows)
    {
        const auto sub = subselectedRows.count (key) > 0;

        if (auto* trackRow = dynamic_cast<Row*> (component.get()))
            trackRow->refresh (! multiSelection.empty() ? multiSelection.count (trackRow->getTrackId()) > 0
                                                        : (! groupSelected && selectedChannels.empty()   // (other rows chosen: no track shows selected)
                                                           && trackRow->getTrackId() == selectedTrack),
                               engine.isTrackArmed (trackRow->getTrackId()), sub);
        else if (auto* folderRow = dynamic_cast<FolderRow*> (component.get()))
        {
            folderRow->setSelected (selectedFolders.count (folderRow->getFolderId()) > 0, sub);
            folderRow->refresh();
        }
        else if (auto* instrumentRow = dynamic_cast<InstrumentRow*> (component.get()))
        {
            instrumentRow->setSelected (selectedInstruments.count (instrumentRow->getInstrumentId()) > 0, sub);
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
    selectedFolders.clear();
    selectedChannels.clear();

    if (const auto tracks = engine.getInstrumentTracks (instrumentId); ! tracks.empty() && onSelect)
        onSelect (tracks.front());

    selectedInstruments = { instrumentId };   // (after onSelect: choosing the track clears it)

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
    selectedInstruments.clear();
    selectedChannels.clear();
    selectedFolders = { folderId };

    if (onSelectionChanged)
        onSelectionChanged (multiSelection);

    if (onGroupSelected)
        onGroupSelected();

    refreshSoon();
}

void TrackList::selectChannel (AudioEngine::AudioChannelId channelId, juce::ModifierKeys)
{
    selectChannels ({ channelId });
}

void TrackList::selectChannels (const std::set<AudioEngine::AudioChannelId>& channels)
{
    multiSelection.clear();
    selectedFolders.clear();
    selectedInstruments.clear();
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

TrackList::RowRef TrackList::refOf (const AudioEngine::SidebarItem& item) const
{
    using K = RowRef::Kind;

    if (item.folder != 0)      return { K::folder, item.folder };
    if (item.instrument != 0)  return { K::instrument, item.instrument };
    if (item.member != 0)      return { K::track, item.member };

    return { engine.isAudioTrack (item.channel) ? K::audioTrack : K::channel, item.channel };
}

bool TrackList::isSelected (RowRef ref) const
{
    switch (ref.kind)
    {
        case RowRef::Kind::folder:      return selectedFolders.count (ref.id) > 0;
        case RowRef::Kind::instrument:  return selectedInstruments.count (ref.id) > 0;
        case RowRef::Kind::track:       return multiSelection.count (ref.id) > 0 || (multiSelection.empty() && selectionCount() == 0 && ref.id == selectedTrack);
        case RowRef::Kind::audioTrack:
        case RowRef::Kind::channel:     return selectedChannels.count (ref.id) > 0;
    }

    return false;
}

int TrackList::selectionCount() const
{
    return (int) (multiSelection.size() + selectedFolders.size() + selectedInstruments.size() + selectedChannels.size());
}

void TrackList::selectOnly (RowRef ref)
{
    switch (ref.kind)
    {
        case RowRef::Kind::folder:      selectFolder (ref.id); break;
        case RowRef::Kind::instrument:  selectInstrument (ref.id); break;
        case RowRef::Kind::audioTrack:
        case RowRef::Kind::channel:     selectChannel (ref.id); break;
        case RowRef::Kind::track:
            multiSelection.clear();

            if (onSelect)
                onSelect (ref.id);

            if (onSelectionChanged)
                onSelectionChanged (multiSelection);
            break;
    }
}

// Ctrl toggles a row (of any kind) in the selection; Shift takes every row from the anchor to here;
// a plain click selects the row alone - or, on a row already in a bigger selection, waits for the
// mouse-up (a drag moves them all)
void TrackList::rowMouseDown (juce::Component*, RowRef ref, const juce::MouseEvent& event)
{
    drag = {};
    clearSelectionOnMouseUp = false;
    pressUsed = false;

    if (event.mods.isCtrlDown())
    {
        // The single selected track (nothing else chosen) becomes the first of several
        if (selectionCount() == 0 && selectedTrack != 0)
            multiSelection.insert (selectedTrack);

        const auto toggle = [&ref] (auto& set) { if (! set.erase (ref.id)) set.insert (ref.id); };

        switch (ref.kind)
        {
            case RowRef::Kind::folder:      toggle (selectedFolders); break;
            case RowRef::Kind::instrument:  toggle (selectedInstruments); break;
            case RowRef::Kind::track:       toggle (multiSelection); break;
            case RowRef::Kind::audioTrack:
            case RowRef::Kind::channel:     toggle (selectedChannels); break;
        }

        anchor = ref;
        pressUsed = true;
    }
    else if (event.mods.isShiftDown() && anchor.id != 0)
    {
        multiSelection.clear();
        selectedFolders.clear();
        selectedInstruments.clear();
        selectedChannels.clear();
        bool inRange = false;

        const auto same = [] (RowRef a, RowRef b) { return a.kind == b.kind && a.id == b.id; };

        for (auto& item : items)
        {
            const auto here = refOf (item);
            const auto isEdge = same (here, anchor) || same (here, ref);

            if (isEdge || inRange)
            {
                switch (here.kind)
                {
                    case RowRef::Kind::folder:      selectedFolders.insert (here.id); break;
                    case RowRef::Kind::instrument:  selectedInstruments.insert (here.id); break;
                    case RowRef::Kind::track:       multiSelection.insert (here.id); break;
                    case RowRef::Kind::audioTrack:
                    case RowRef::Kind::channel:     selectedChannels.insert (here.id); break;
                }
            }

            if (isEdge)
            {
                if (inRange || same (anchor, ref))
                    break;          // closing edge (or a one-row range)

                inRange = true;     // opening edge
            }
        }

        pressUsed = true;
    }
    else
    {
        if (isSelected (ref) && selectionCount() > 1)
            clearSelectionOnMouseUp = true;   // (resolved on mouse-up, unless a drag moves them all)
        else if (ref.kind == RowRef::Kind::track || ref.kind == RowRef::Kind::audioTrack || ref.kind == RowRef::Kind::channel)
        {
            selectOnly (ref);   // (a folder or an instrument: on mouse-up - its arrow opens it instead)
            pressUsed = true;
        }

        anchor = ref;
    }

    if (pressUsed && (event.mods.isCtrlDown() || event.mods.isShiftDown()))
    {
        if (onSelectionChanged)
            onSelectionChanged (multiSelection);

        if (onGroupSelected && (! selectedFolders.empty() || ! selectedInstruments.empty() || ! selectedChannels.empty()))
            onGroupSelected();
    }

    refresh();
}

// What a drag from 'pressed' moves: the selection when it's in it (else just that row), in visual
// order; what's inside a selected folder or instrument goes with it (not on its own); an
// instrument's tracks stand for the instrument - unless only tracks of one instrument are dragged
std::vector<AudioEngine::TreeNode> TrackList::draggedNodes (RowRef pressed) const
{
    using N = AudioEngine::TreeNode;
    std::vector<N> nodes;

    const auto add = [&nodes] (N node)
    {
        if (std::find (nodes.begin(), nodes.end(), node) == nodes.end())
            nodes.push_back (node);
    };

    const auto nodeOf = [this] (RowRef ref) -> std::optional<N>
    {
        switch (ref.kind)
        {
            case RowRef::Kind::folder:      return N { N::Kind::folder, ref.id };
            case RowRef::Kind::instrument:  return N { N::Kind::instrument, ref.id };
            case RowRef::Kind::track:       return N { N::Kind::track, ref.id };
            case RowRef::Kind::audioTrack:  return N { N::Kind::audioTrack, ref.id };
            case RowRef::Kind::channel:
                if (const auto instrument = engine.getAudioChannelInput (ref.id); instrument != 0)
                    return N { N::Kind::instrument, instrument };   // (an instrument's output: the instrument)
                break;
        }

        return std::nullopt;
    };

    if (! isSelected (pressed) || selectionCount() <= 1)
    {
        if (auto node = nodeOf (pressed))
            add (*node);
    }
    else
    {
        int insideDepth = -1;   // inside a selected folder or instrument: it goes with that

        for (auto& item : items)
        {
            if (insideDepth >= 0 && item.depth <= insideDepth)
                insideDepth = -1;

            if (insideDepth >= 0)
                continue;

            const auto ref = refOf (item);

            if (! isSelected (ref))
                continue;

            if (ref.kind == RowRef::Kind::folder || ref.kind == RowRef::Kind::instrument)
                insideDepth = item.depth;

            if (auto node = nodeOf (ref))
                add (*node);
        }
    }

    // Tracks of one instrument alone: reordered in it. Otherwise an instrument's tracks: the instrument
    AudioEngine::InstrumentId only = 0;
    auto allOfOne = ! nodes.empty();

    for (auto& node : nodes)
    {
        const auto instrument = node.kind == N::Kind::track ? engine.getTrackInstrument (node.id) : 0;
        allOfOne = allOfOne && instrument != 0 && (only == 0 || only == instrument);
        only = instrument;
    }

    if (allOfOne)
        return nodes;

    std::vector<N> result;

    for (auto node : nodes)
    {
        if (node.kind == N::Kind::track)
            if (const auto instrument = engine.getTrackInstrument (node.id); instrument != 0)
                node = { N::Kind::instrument, instrument };

        if (std::find (result.begin(), result.end(), node) == result.end())
            result.push_back (node);
    }

    return result;
}

void TrackList::rowMouseDrag (juce::Component* row, RowRef ref, const juce::MouseEvent& event)
{
    // "Moving only happens when the mouse moves outside of the channel being dragged"
    if (! drag.active && row->getLocalBounds().contains (event.getPosition()))
        return;

    if (! drag.active)
    {
        drag.nodes = draggedNodes (ref);

        if (drag.nodes.empty())
            return;

        drag.active = true;
        drag.reorderIn = 0;

        if (drag.nodes.front().kind == AudioEngine::TreeNode::Kind::track)
            if (const auto instrument = engine.getTrackInstrument (drag.nodes.front().id);
                instrument != 0 && std::all_of (drag.nodes.begin(), drag.nodes.end(), [&] (auto& n)
                                                { return n.kind == AudioEngine::TreeNode::Kind::track && engine.getTrackInstrument (n.id) == instrument; }))
                drag.reorderIn = instrument;
    }

    computeDropTarget (event.getEventRelativeTo (&rowContainer).getPosition().y);
    rowContainer.repaint();
}

void TrackList::computeDropTarget (int y)
{
    using N = AudioEngine::TreeNode;
    drag.valid = false;
    drag.intoFolder = false;
    drag.indicatorY = -1;
    drag.folderHighlight = {};

    const auto isDragged = [this] (const AudioEngine::SidebarItem& item)
    {
        const auto has = [this] (N node) { return std::find (drag.nodes.begin(), drag.nodes.end(), node) != drag.nodes.end(); };

        if (item.folder != 0)       return has ({ N::Kind::folder, item.folder });
        if (item.instrument != 0)   return has ({ N::Kind::instrument, item.instrument });
        if (item.member != 0)       return has ({ N::Kind::track, item.member });

        return item.channel != 0 && has ({ N::Kind::audioTrack, item.channel });
    };

    // The row under the mouse
    int rowY = 0;
    size_t i = 0;

    for (; i < items.size(); ++i)
    {
        const auto height = heightOfItem (items[i]);

        if (y < rowY + height)
            break;

        rowY += height;
    }

    if (drag.reorderIn != 0)   // an instrument's tracks, among its own tracks
    {
        if (i >= items.size() || items[i].member == 0 || engine.getTrackInstrument (items[i].member) != drag.reorderIn)
            return;

        const auto before = y < rowY + heightOfItem (items[i]) / 2;
        const auto target = items[i].member;
        std::vector<AudioEngine::TrackId> moved, order;

        for (auto& node : drag.nodes)
            moved.push_back (node.id);

        for (auto track : engine.getInstrumentTracks (drag.reorderIn))
        {
            const auto isMoved = std::find (moved.begin(), moved.end(), track) != moved.end();

            if (track == target && before)
                order.insert (order.end(), moved.begin(), moved.end());

            if (! isMoved)
                order.push_back (track);

            if (track == target && ! before)
                order.insert (order.end(), moved.begin(), moved.end());
        }

        if (std::find (moved.begin(), moved.end(), target) != moved.end())
            return;   // (onto itself: nothing)

        drag.valid = true;
        drag.reordered = order;
        drag.indicatorY = before ? rowY : rowY + heightOfItem (items[i]);
        return;
    }

    // The parent's slots before item index i (dragged ones don't count: the engine takes them out first)
    const auto slotIndexAt = [&] (AudioEngine::FolderId parent, size_t end)
    {
        int index = 0;

        for (size_t j = 0; j < end; ++j)
            if (items[j].parent == parent && engine.isTreeSlot (items[j]) && ! isDragged (items[j]))
                ++index;

        return index;
    };

    if (i >= items.size())
    {
        // Below every row: append at the top level
        drag.valid = true;
        drag.parent = 0;
        drag.index = slotIndexAt (0, items.size());
        drag.indicatorY = rowY;
        return;
    }

    // Inside an instrument (its tracks, its audio): around the instrument (its slot)
    auto at = i;
    auto atY = rowY;

    if (! engine.isTreeSlot (items[at]))
        while (at > 0 && items[at].instrument == 0)
        {
            --at;
            atY -= heightOfItem (items[at]);
        }

    const auto& item = items[at];
    const auto height = heightOfItem (item);

    // A folder row's middle drops INTO the folder (its edges insert around it)
    if (item.folder != 0 && at == i && y >= rowY + height / 4 && y <= rowY + 3 * height / 4)
    {
        if (isDragged (item))
            return;   // not into itself (subtrees are rejected by the engine on drop)

        drag.valid = true;
        drag.intoFolder = true;
        drag.parent = item.folder;
        drag.index = std::numeric_limits<int>::max();   // append
        drag.folderHighlight = { 0, rowTops[i], rowContainer.getWidth(), height };
        return;
    }

    auto before = at == i && y < rowY + height / 2;
    auto indicator = before ? atY : atY + height;

    if (! before && item.instrument != 0)   // after an instrument: after its tracks and audio
        for (auto j = at + 1; j < items.size() && items[j].depth > item.depth; ++j)
            indicator += heightOfItem (items[j]);

    drag.valid = true;
    drag.parent = item.parent;
    drag.index = slotIndexAt (item.parent, at) + (before || isDragged (item) ? 0 : 1);
    drag.indicatorY = indicator;
}

bool TrackList::finishRowDrag (RowRef ref)
{
    const auto wasDragging = drag.active;
    auto used = wasDragging || pressUsed;

    if (drag.active && drag.valid)
    {
        if (drag.reorderIn != 0)
            engine.reorderInstrumentTracks (drag.reorderIn, drag.reordered);
        else
            engine.moveTreeNodes (drag.nodes, drag.parent, drag.index);
    }
    else if (! wasDragging && clearSelectionOnMouseUp)
    {
        // The deferred plain click on a selected row: now it alone becomes the selection
        selectOnly (ref);
        used = true;
    }

    clearSelectionOnMouseUp = false;
    pressUsed = false;
    drag = {};
    rowContainer.repaint();
    refreshSoon();   // rebuilding rows would delete the row we're called from
    return used;
}

void TrackList::refreshSoon()
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TrackList> (this)]
                                     { if (safe != nullptr) safe->refresh(); });
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

    // Its colour: its regions', and a group's tag (copied from its first instrument when it was grouped)
    menu.addSubMenu ("Color", colours::buildMenu (engine.getFolderColour (folderId), [safe, folderId] (juce::String hex)
        {
            if (safe != nullptr)
            {
                safe->engine.setFolderColour (folderId, hex);
                safe->refresh();
            }
        }));
    menu.addSeparator();
    menu.addItem ("Remove folder and everything in it...", [safe, folderId]
    {
        if (safe != nullptr && safe->onRemoveFolder)
            safe->onRemoveFolder (folderId);
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
