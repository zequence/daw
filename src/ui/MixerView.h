#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"
#include "MixerParts.h"
#include "CloseButton.h"

// The mixer (MILESTONES.md "Audio mixer"), in a console style (SSL first). A strip per audio
// channel in the sidebar's folder order, then the six Aux buses, the master at the right. A
// channel strip, top to bottom: name (double-click to rename), inserts, EQ (SSL 4000 E: filters,
// HF / HMF / LMF / LF, bell switches, EQ in), dynamics (threshold, ratio, attack, release,
// make-up, in), six aux sends (one per Aux bus), pan, fader with meter, solo / mute, output.
// Working now: name, pan, fader, meter, solo, mute (and the master). The inserts, EQ, dynamics,
// aux knobs, the Aux buses and the output are placeholders: they show and turn, nothing more.
class MixerView final : public juce::Component, private juce::Timer
{
public:
    explicit MixerView (AudioEngine& e) : engine (e)
    {
        title.setText ("Mixer", juce::dontSendNotification);
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (title);
        addAndMakeVisible (closeButton);

        styleBox.addItem (mixer::ConsoleStyle::ssl().name, 1);
        styleBox.setSelectedId (1, juce::dontSendNotification);
        styleBox.setTooltip ("Console style - SSL for now; more styles to come");
        styleBox.setWantsKeyboardFocus (false);
        addAndMakeVisible (styleBox);

        // Strips scroll sideways; the whole row scrolls up and down when the window is short
        outer.setViewedComponent (&body, false);
        outer.setScrollBarsShown (true, false);
        addAndMakeVisible (outer);

        channelsViewport.setViewedComponent (&strips, false);
        channelsViewport.setScrollBarsShown (false, true);
        body.addAndMakeVisible (channelsViewport);

        for (int aux = 1; aux <= 6; ++aux)
        {
            auto strip = std::make_unique<Strip> (*this, Kind::aux, 0, aux);
            strips.addAndMakeVisible (*strip);
            auxStrips.push_back (std::move (strip));
        }

        master = std::make_unique<Strip> (*this, Kind::master, 0, 0);
        body.addAndMakeVisible (*master);

        startTimerHz (30);
    }

    CloseButton closeButton;

    // The selected track's channel is highlighted (0 = none)
    void setHighlightedChannel (AudioEngine::AudioChannelId id)
    {
        if (id != highlighted)
        {
            highlighted = id;

            for (auto& strip : channelStrips)
                strip->repaint();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (30).reduced (8, 3);
        closeButton.setBounds (header.removeFromRight (header.getHeight() + 4));
        header.removeFromRight (8);
        styleBox.setBounds (header.removeFromRight (90));
        title.setBounds (header);

        outer.setBounds (area);
        layoutBody();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceContent));
    }

private:
    enum class Kind { channel, aux, master };
    static constexpr int stripWidth = 100, stripHeight = 1040;

    const mixer::ConsoleStyle& style() const   { return mixer::ConsoleStyle::ssl(); }

    //==========================================================================
    // A section of a strip: a panel with a legend; its controls are laid out by the strip
    struct Section final : juce::Component
    {
        explicit Section (const juce::String& legendToUse) : legend (legendToUse) {}

        void paint (juce::Graphics& g) override
        {
            const auto& style = mixer::ConsoleStyle::ssl();
            g.setColour (style.section);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
            g.setColour (style.sectionLine);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);
            g.setColour (style.sectionText);
            g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
            g.drawText (legend, getLocalBounds().removeFromTop (12), juce::Justification::centred, false);
        }

        juce::String legend;
    };

    //==========================================================================
    struct Strip final : juce::Component
    {
        Strip (MixerView& o, Kind kindToUse, AudioEngine::AudioChannelId id, int auxNumberToUse)
            : owner (o), kind (kindToUse), channelId (id), auxNumber (auxNumberToUse)
        {
            const auto& style = owner.style();
            const auto placeholderTip = juce::String ("Placeholder - not working yet (MILESTONES.md \"Audio mixer\")");

            name.setJustificationType (juce::Justification::centred);
            name.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            name.setEditable (false, kind == Kind::channel);   // double-click renames a channel
            name.setTooltip (kind == Kind::channel ? "Double-click to rename" : kind == Kind::master ? "The master bus" : placeholderTip);
            name.onTextChange = [this]
            {
                owner.engine.setAudioChannelName (channelId, name.getText());
                refreshName();
            };
            addAndMakeVisible (name);

            inserts.setTooltip (placeholderTip);
            inserts.slots = kind == Kind::channel ? 16 : 8;   // channels 16 inserts, Aux buses and the master 8
            addChildComponent (inserts);

            // The inserts sit behind the EQ and dynamics: this flips between them
            flip.setTooltip ("Show the inserts (in place of the EQ and dynamics) - click again for the EQ");
            flip.onClick = [this] { showInserts (flip.getToggleState()); };

            if (kind != Kind::master)
                addAndMakeVisible (flip);
            else
                inserts.setVisible (true);   // the master has no EQ or dynamics: its inserts always show

            // --- EQ (SSL 4000 E) and dynamics: placeholders (channels and Aux buses) ---
            if (kind != Kind::master)
            {
                const auto add = [this, &placeholderTip] (mixer::Knob& knob, Section& section, double min, double max,
                                                          double initial, std::function<juce::String (double)> format)
                {
                    knob.setRange (min, max, (max - min) / 200.0);
                    knob.setValue (initial, juce::dontSendNotification);
                    knob.setDoubleClickReturnValue (true, initial);
                    knob.format = std::move (format);
                    knob.setTooltip (knob.legend + " - " + placeholderTip);
                    section.addAndMakeVisible (knob);
                };

                const auto db = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1); };
                const auto hz = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 1) + "k" : juce::String (juce::roundToInt (v)); };
                const auto plain = [] (double v) { return juce::String (v, 1); };
                const auto ms = [] (double v) { return v < 10.0 ? juce::String (v, 1) : juce::String (juce::roundToInt (v)); };

                add (hpf, eq, 16.0, 350.0, 16.0, hz);       hpf.setSkewFactorFromMidPoint (60.0);
                add (lpf, eq, 3000.0, 22000.0, 22000.0, hz); lpf.setSkewFactorFromMidPoint (8000.0);
                add (hfGain, eq, -15.0, 15.0, 0.0, db);
                add (hfFreq, eq, 1500.0, 16000.0, 8000.0, hz);
                add (hmfGain, eq, -15.0, 15.0, 0.0, db);
                add (hmfFreq, eq, 600.0, 7000.0, 2000.0, hz);
                add (hmfQ, eq, 0.5, 3.0, 1.0, plain);
                add (lmfGain, eq, -15.0, 15.0, 0.0, db);
                add (lmfFreq, eq, 200.0, 2000.0, 600.0, hz);
                add (lmfQ, eq, 0.5, 3.0, 1.0, plain);
                add (lfGain, eq, -15.0, 15.0, 0.0, db);
                add (lfFreq, eq, 30.0, 450.0, 100.0, hz);

                add (threshold, dynamics, -40.0, 0.0, 0.0, db);
                add (ratio, dynamics, 1.0, 20.0, 1.0, [] (double v) { return juce::String (v, 1) + ":1"; });
                add (attack, dynamics, 0.1, 100.0, 10.0, ms);   attack.setSkewFactorFromMidPoint (10.0);
                add (release, dynamics, 50.0, 2000.0, 300.0, ms); release.setSkewFactorFromMidPoint (300.0);
                add (makeup, dynamics, 0.0, 20.0, 0.0, db);

                for (auto* b : { &hfBell, &lfBell, &eqIn })
                {
                    b->setTooltip (placeholderTip);
                    eq.addAndMakeVisible (b);
                }

                dynamicsIn.setTooltip (placeholderTip);
                dynamics.addAndMakeVisible (dynamicsIn);
                addAndMakeVisible (eq);
                addAndMakeVisible (dynamics);
            }

            // --- Six aux sends, one per Aux bus (channels only): placeholders ---
            if (kind == Kind::channel)
            {
                for (int i = 0; i < 6; ++i)
                {
                    auto knob = std::make_unique<mixer::Knob> ("Aux " + juce::String (i + 1), style.auxCap, false);
                    knob->setRange (-60.0, 6.0, 0.1);
                    knob->setValue (-60.0, juce::dontSendNotification);
                    knob->setSkewFactorFromMidPoint (-12.0);
                    knob->format = [] (double v) { return v <= -59.9 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (v, 1); };
                    knob->setTooltip ("Send to Aux " + juce::String (i + 1) + " - " + placeholderTip);
                    aux.addAndMakeVisible (*knob);
                    auxKnobs.push_back (std::move (knob));
                }

                addAndMakeVisible (aux);
            }

            // --- Drive (by the level): a placeholder ---
            drive.setRange (0.0, 10.0, 0.05);
            drive.setDoubleClickReturnValue (true, 0.0);
            drive.format = [] (double v) { return juce::String (v, 1); };
            drive.setTooltip ("Drive - " + placeholderTip);

            if (kind != Kind::master)
                addAndMakeVisible (drive);

            // --- Pan, fader, meter, solo, mute: working on channels and the master ---
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.alwaysShowValue = true;
            pan.format = [] (double v)
            {
                if (std::abs (v) < 0.005) return juce::String ("0");
                if (v >= 0.995)           return juce::String ("1");
                if (v <= -0.995)          return juce::String ("-1");
                return juce::String (v, 2);
            };
            pan.setTooltip (kind == Kind::aux ? placeholderTip : "Pan (double-click: centre)");
            pan.onValueChange = [this]
            {
                if (kind == Kind::channel)
                    owner.engine.setAudioChannelPan (channelId, (float) pan.getValue());
            };

            if (kind != Kind::master)
                addAndMakeVisible (pan);

            fader.setRange (-60.0, 6.0, 0.1);
            fader.setSkewFactorFromMidPoint (-12.0);
            fader.setDoubleClickReturnValue (true, 0.0);
            fader.setValue (0.0, juce::dontSendNotification);
            fader.setTooltip (kind == Kind::aux ? placeholderTip : "Level (double-click: 0 dB)");
            fader.onValueChange = [this]
            {
                if (auto* p = processor())
                    p->setGain (fader.getValue() <= -59.9 ? 0.0f : juce::Decibels::decibelsToGain ((float) fader.getValue()));

                updateLevelText();
            };
            addAndMakeVisible (fader);
            addAndMakeVisible (meter);

            level.setJustificationType (juce::Justification::centred);
            level.setFont (juce::FontOptions (11.0f));
            level.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
            addAndMakeVisible (level);

            for (auto* b : { &solo, &mute })
            {
                b->setClickingTogglesState (true);
                b->setWantsKeyboardFocus (false);
                addAndMakeVisible (b);
            }

            theme::setButtonRole (solo, "solo");
            theme::setButtonRole (mute, "mute");
            solo.setVisible (kind != Kind::master);
            solo.onClick = [this]
            {
                if (kind == Kind::channel)
                    owner.engine.setAudioChannelSoloed (channelId, solo.getToggleState());
            };
            mute.onClick = [this]
            {
                if (auto* p = processor())
                    p->setMuted (mute.getToggleState());
            };

            output.setTooltip (kind == Kind::master ? "The audio device" : placeholderTip);
            output.setText (kind == Kind::master ? juce::String ("Device out") : juce::String::fromUTF8 ("\xe2\x86\x92 Master"),
                            juce::dontSendNotification);
            output.setJustificationType (juce::Justification::centred);
            output.setFont (juce::FontOptions (10.5f));
            output.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
            output.setColour (juce::Label::outlineColourId, juce::Colours::white.withAlpha (0.12f));
            addAndMakeVisible (output);

            refreshName();
            syncControls();
            updateLevelText();
        }

        void showInserts (bool insertsShown)
        {
            inserts.setVisible (insertsShown || kind == Kind::master);
            eq.setVisible (! insertsShown && kind != Kind::master);
            dynamics.setVisible (! insertsShown && kind != Kind::master);
        }

        // The engine's strip (none for the Aux buses yet)
        AudioChannelProcessor* processor() const
        {
            if (kind == Kind::master)  return owner.engine.getMasterChannel();
            if (kind == Kind::channel) return owner.engine.getAudioChannel (channelId);
            return nullptr;
        }

        void refreshName()
        {
            if (name.isBeingEdited())
                return;

            name.setText (kind == Kind::master ? juce::String ("Master")
                            : kind == Kind::aux ? "Aux " + juce::String (auxNumber)
                                                : owner.engine.getAudioChannelName (channelId),
                          juce::dontSendNotification);
        }

        void updateLevelText()
        {
            level.setText (fader.getValue() <= -59.9 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e")) : juce::String (fader.getValue(), 1),
                           juce::dontSendNotification);
        }

        // The controls follow the engine (an API change, undo) - not while being dragged
        void syncControls()
        {
            if (auto* p = processor())
            {
                if (! fader.isMouseButtonDown())
                    fader.setValue (p->getGain() <= 0.0f ? -60.0 : juce::Decibels::gainToDecibels ((double) p->getGain(), -60.0),
                                    juce::dontSendNotification);

                if (! pan.isMouseButtonDown())
                    pan.setValue (p->getPan(), juce::dontSendNotification);

                mute.setToggleState (p->isMuted(), juce::dontSendNotification);
                updateLevelText();
            }

            if (kind == Kind::channel)
                solo.setToggleState (owner.engine.isAudioChannelSoloed (channelId), juce::dontSendNotification);
        }

        void tick()
        {
            if (auto* p = processor())
                meter.update (p->getLastPeak(), p->getLastRms());
        }

        void paint (juce::Graphics& g) override
        {
            const auto isHighlighted = kind == Kind::channel && channelId == owner.highlighted;
            g.setColour (isHighlighted ? owner.style().panel.brighter (0.25f) : owner.style().panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

            if (kind != Kind::channel)   // Aux and master: a coloured band under the name
            {
                g.setColour (kind == Kind::master ? juce::Colour (0xffc23b33) : owner.style().auxCap);
                g.fillRect (getLocalBounds().reduced (4, 0).withTop (27).withHeight (2));
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (4);
            name.setBounds (area.removeFromTop (22));
            area.removeFromTop (4);

            if (kind == Kind::master)
            {
                inserts.setBounds (area.removeFromTop (150));
                area.removeFromTop (5);
            }
            else
            {
                flip.setBounds (area.removeFromTop (16).withSizeKeepingCentre (64, 15));
                area.removeFromTop (4);
            }

            // Two knobs side by side per row
            const auto row = [] (juce::Rectangle<int>& inside, int height, juce::Component* left, juce::Component* right)
            {
                auto line = inside.removeFromTop (height);

                if (right == nullptr)
                {
                    left->setBounds (line.withSizeKeepingCentre (line.getWidth() / 2, height));
                    return;
                }

                left->setBounds (line.removeFromLeft (line.getWidth() / 2));
                right->setBounds (line);
            };
            constexpr int knobRow = 48, buttonRow = 16;

            if (kind != Kind::master)
            {
                // The EQ and dynamics - or, flipped, the inserts in the same space
                const auto pageTop = area.getY();
                auto e = area.removeFromTop (12 + 6 * knobRow + knobRow + 3 * buttonRow + 8);
                eq.setBounds (e);
                auto inside = eq.getLocalBounds().reduced (2).withTrimmedTop (12);
                row (inside, knobRow, &hpf, &lpf);
                row (inside, knobRow, &hfGain, &hfFreq);
                hfBell.setBounds (inside.removeFromTop (buttonRow).withSizeKeepingCentre (44, buttonRow - 2));
                row (inside, knobRow, &hmfGain, &hmfFreq);
                row (inside, knobRow, &hmfQ, nullptr);
                row (inside, knobRow, &lmfGain, &lmfFreq);
                row (inside, knobRow, &lmfQ, nullptr);
                row (inside, knobRow, &lfGain, &lfFreq);
                lfBell.setBounds (inside.removeFromTop (buttonRow).withSizeKeepingCentre (44, buttonRow - 2));
                eqIn.setBounds (inside.removeFromTop (buttonRow + 2).withSizeKeepingCentre (48, buttonRow - 1));
                area.removeFromTop (5);

                auto d = area.removeFromTop (12 + 3 * knobRow + buttonRow + 6);
                dynamics.setBounds (d);
                inside = dynamics.getLocalBounds().reduced (2).withTrimmedTop (12);
                row (inside, knobRow, &threshold, &ratio);
                row (inside, knobRow, &attack, &release);
                row (inside, knobRow, &makeup, nullptr);
                dynamicsIn.setBounds (inside.removeFromTop (buttonRow + 2).withSizeKeepingCentre (48, buttonRow - 1));
                inserts.setBounds (getLocalBounds().reduced (4).withTop (pageTop).withBottom (dynamics.getBottom()));
                area.removeFromTop (5);
            }

            if (kind == Kind::channel)
            {
                auto a = area.removeFromTop (12 + 3 * knobRow + 4);
                aux.setBounds (a);
                auto inside = aux.getLocalBounds().reduced (2).withTrimmedTop (12);

                for (size_t i = 0; i + 1 < auxKnobs.size(); i += 2)
                    row (inside, knobRow, auxKnobs[i].get(), auxKnobs[i + 1].get());

                area.removeFromTop (5);
            }

            output.setBounds (area.removeFromBottom (20));
            area.removeFromBottom (4);
            auto buttons = area.removeFromBottom (22);
            solo.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
            mute.setBounds (buttons.reduced (1, 0));
            area.removeFromBottom (4);
            level.setBounds (area.removeFromBottom (16));

            if (kind != Kind::master)   // drive and pan, side by side, by the level
            {
                auto knobs = area.removeFromTop (knobRow);
                drive.setBounds (knobs.removeFromLeft (knobs.getWidth() / 2));
                pan.setBounds (knobs);
                area.removeFromTop (4);
            }

            auto faderArea = area.withHeight (juce::jmin (area.getHeight(), 150)).reduced (0, 2);   // a shorter fader
            meter.setBounds (faderArea.removeFromRight (12));
            faderArea.removeFromRight (4);
            fader.setBounds (faderArea);
        }

        MixerView& owner;
        const Kind kind;
        const AudioEngine::AudioChannelId channelId;   // channels
        const int auxNumber;                           // Aux buses: 1-6
        juce::Label name, level, output;
        mixer::Placeholder inserts { "Inserts", 8 };
        mixer::LitButton flip { "INSERTS", juce::Colour (0xff9fb3c8) };
        mixer::Knob drive { "DRIVE", owner.style().driveCap, false };

        Section eq { "EQ" }, dynamics { "DYNAMICS" }, aux { "AUX" };
        mixer::Knob hpf { "HPF", owner.style().filterCap, false }, lpf { "LPF", owner.style().filterCap, false },
                    hfGain { "HF dB", owner.style().hfCap, true }, hfFreq { "HF kHz", owner.style().hfCap, false },
                    hmfGain { "HMF dB", owner.style().hmfCap, true }, hmfFreq { "HMF kHz", owner.style().hmfCap, false },
                    hmfQ { "HMF Q", owner.style().hmfCap, false },
                    lmfGain { "LMF dB", owner.style().lmfCap, true }, lmfFreq { "LMF kHz", owner.style().lmfCap, false },
                    lmfQ { "LMF Q", owner.style().lmfCap, false },
                    lfGain { "LF dB", owner.style().lfCap, true }, lfFreq { "LF Hz", owner.style().lfCap, false },
                    threshold { "THRESH", owner.style().dynamicsCap, false }, ratio { "RATIO", owner.style().dynamicsCap, false },
                    attack { "ATTACK", owner.style().dynamicsCap, false }, release { "RELEASE", owner.style().dynamicsCap, false },
                    makeup { "MAKE-UP", owner.style().dynamicsCap, false };
        mixer::LitButton hfBell { "BELL", owner.style().bellLit }, lfBell { "BELL", owner.style().bellLit },
                         eqIn { "EQ IN", owner.style().eqLit }, dynamicsIn { "DYN IN", owner.style().dynamicsLit };
        std::vector<std::unique_ptr<mixer::Knob>> auxKnobs;

        mixer::Knob pan { {}, owner.style().panCap, true };
        mixer::LevelFader fader;
        mixer::Meter meter;
        juce::TextButton solo { "S" }, mute { "M" };
    };

    //==========================================================================
    void timerCallback() override
    {
        // The channels in the sidebar's (folder) order; strips rebuild when that changes
        std::vector<AudioEngine::AudioChannelId> ids;

        for (auto& item : engine.getSidebarItems (false, false))
            if (item.member != 0)
                ids.push_back ((AudioEngine::AudioChannelId) item.member);

        if (ids != shownIds)
        {
            shownIds = ids;
            channelStrips.clear();

            for (auto id : ids)
            {
                auto strip = std::make_unique<Strip> (*this, Kind::channel, id, 0);
                strips.addAndMakeVisible (*strip);
                channelStrips.push_back (std::move (strip));
            }

            layoutBody();
        }

        const auto revisionChanged = engine.getStateRevision() != lastRevision;
        lastRevision = engine.getStateRevision();
        const auto sync = revisionChanged || ++syncCounter % 8 == 0;

        for (auto& strip : channelStrips)
        {
            strip->tick();

            if (sync)
            {
                strip->syncControls();
                strip->refreshName();
            }
        }

        master->tick();
        master->syncControls();
    }

    // The row: channels and Aux buses scrolling sideways, the master fixed at the right; the
    // whole row scrolls up and down when the view is shorter than a strip
    void layoutBody()
    {
        const auto visibleHeight = outer.getMaximumVisibleHeight();
        const auto height = juce::jmax (stripHeight, visibleHeight);
        const auto width = outer.getMaximumVisibleWidth();
        body.setSize (width, height);

        master->setBounds (width - stripWidth - 8, 4, stripWidth, height - 8);
        channelsViewport.setBounds (4, 0, width - stripWidth - 20, height);

        const auto stripsHeight = height - (channelsViewport.isHorizontalScrollBarShown() ? 12 : 0);
        const auto gap = 14;   // between the channels and the Aux buses
        const auto count = (int) channelStrips.size();
        strips.setSize (count * (stripWidth + 4) + gap + 6 * (stripWidth + 4) + 4, stripsHeight);

        for (int i = 0; i < count; ++i)
            channelStrips[(size_t) i]->setBounds (4 + i * (stripWidth + 4), 4, stripWidth, stripsHeight - 8);

        for (int i = 0; i < (int) auxStrips.size(); ++i)
            auxStrips[(size_t) i]->setBounds (4 + count * (stripWidth + 4) + gap + i * (stripWidth + 4), 4, stripWidth, stripsHeight - 8);
    }

    AudioEngine& engine;
    juce::Label title;
    juce::ComboBox styleBox;
    juce::Viewport outer, channelsViewport;
    juce::Component body, strips;
    std::vector<std::unique_ptr<Strip>> channelStrips, auxStrips;
    std::unique_ptr<Strip> master;
    std::vector<AudioEngine::AudioChannelId> shownIds;
    AudioEngine::AudioChannelId highlighted = 0;
    int lastRevision = -1, syncCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
