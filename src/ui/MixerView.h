#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"
#include "ThemedLookAndFeel.h"
#include "CloseButton.h"

// The mixer (MILESTONES.md "Audio mixer", phase 1): a strip per audio channel in the sidebar's
// folder order, the master at the right. Each strip: name (double-click to rename), inserts and
// sends (placeholders until those phases), pan, fader with a meter (peak and RMS, peak hold, a
// clip light - click to reset), solo, mute, output (a placeholder: the master for now).
class MixerView final : public juce::Component, private juce::Timer
{
public:
    explicit MixerView (AudioEngine& e) : engine (e)
    {
        title.setText ("Mixer", juce::dontSendNotification);
        title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (title);
        addAndMakeVisible (closeButton);

        viewport.setViewedComponent (&strips, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);

        master = std::make_unique<Strip> (*this, 0);
        addAndMakeVisible (*master);

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
        title.setBounds (header);

        master->setBounds (area.removeFromRight (stripWidth + 8).withTrimmedLeft (8).reduced (0, 4));
        viewport.setBounds (area.reduced (4, 0));
        layoutStrips();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceContent));
        g.setColour (theme::colour (theme::Token::borderSubtle));
        g.fillRect (master->getX() - 5, master->getY(), 1, master->getHeight());
    }

private:
    static constexpr int stripWidth = 78;

    //==========================================================================
    // A level meter: peak (bright) over RMS (body), a peak-hold line, a clip light (click resets)
    struct Meter final : juce::Component
    {
        void update (float newPeak, float newRms)
        {
            peak = juce::jmax (newPeak, peak * 0.86f);
            rms = juce::jmax (newRms, rms * 0.9f);

            if (newPeak >= holdLevel)
            {
                holdLevel = newPeak;
                holdTicks = 45;   // ~1.5 s at 30 Hz
            }
            else if (--holdTicks <= 0)
            {
                holdLevel = juce::jmax (0.0f, holdLevel * 0.9f);
            }

            clipped = clipped || newPeak >= 1.0f;
            repaint();
        }

        static float toFraction (float level)
        {
            const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
            return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);   // -60 .. +6 dB
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().toFloat();
            const auto clip = area.removeFromTop (5.0f);
            g.setColour (clipped ? juce::Colour (0xffe0403a) : juce::Colour (0xff2a2d33));
            g.fillRect (clip.reduced (0.0f, 1.0f));

            g.setColour (juce::Colour (0xff15171a));
            g.fillRect (area);

            const auto barFor = [&area] (float level) { return area.withTop (area.getBottom() - area.getHeight() * toFraction (level)); };
            g.setColour (juce::Colour (0xff3d8f5a));
            g.fillRect (barFor (rms));
            g.setColour (juce::Colour (0xff6fd08f).withAlpha (0.55f));
            g.fillRect (barFor (peak).withBottom (barFor (rms).getY()));

            if (holdLevel > 0.001f)
            {
                g.setColour (holdLevel >= 1.0f ? juce::Colour (0xffe0403a) : juce::Colours::white.withAlpha (0.8f));
                g.fillRect (area.getX(), barFor (holdLevel).getY(), area.getWidth(), 1.5f);
            }
        }

        void mouseDown (const juce::MouseEvent&) override   { clipped = false; holdLevel = 0.0f; repaint(); }

        float peak = 0.0f, rms = 0.0f, holdLevel = 0.0f;
        int holdTicks = 0;
        bool clipped = false;
    };

    //==========================================================================
    // A grey box with a caption: a part of the strip that isn't built yet
    struct Placeholder final : juce::Component, juce::SettableTooltipClient
    {
        Placeholder (const juce::String& captionToUse, int slotsToUse) : caption (captionToUse), slots (slotsToUse) {}

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (caption, area.removeFromTop (13.0f).toNearestInt(), juce::Justification::centredLeft, false);

            for (int i = 0; i < slots; ++i)
            {
                const auto slot = area.removeFromTop (area.getHeight() / (float) (slots - i)).reduced (0.0f, 1.0f);
                g.setColour (juce::Colour (0xff202328));
                g.fillRoundedRectangle (slot, theme::corner);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.drawRoundedRectangle (slot, theme::corner, 1.0f);
            }
        }

        juce::String caption;
        int slots;
    };

    //==========================================================================
    struct Strip final : juce::Component
    {
        Strip (MixerView& o, AudioEngine::AudioChannelId id) : owner (o), channelId (id)
        {
            name.setJustificationType (juce::Justification::centred);
            name.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            name.setEditable (false, channelId != 0);   // double-click renames (not the master)
            name.setTooltip (channelId != 0 ? "Double-click to rename" : "The master bus");
            name.onTextChange = [this]
            {
                owner.engine.setAudioChannelName (channelId, name.getText());
                refreshName();
            };
            addAndMakeVisible (name);

            inserts.setTooltip ("Insert effects - coming (MILESTONES.md \"Audio mixer\", phase 4)");
            sends.setTooltip ("Aux sends - coming (phase 3)");
            addAndMakeVisible (inserts);

            if (channelId != 0)
                addAndMakeVisible (sends);

            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setTooltip ("Pan (double-click: centre)");
            pan.onValueChange = [this] { owner.engine.setAudioChannelPan (channelId, (float) pan.getValue()); };

            if (channelId != 0)
                addAndMakeVisible (pan);

            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            fader.setRange (-60.0, 6.0, 0.1);
            fader.setSkewFactorFromMidPoint (-12.0);
            fader.setDoubleClickReturnValue (true, 0.0);
            fader.setTooltip ("Level (double-click: 0 dB)");
            fader.onValueChange = [this]
            {
                if (auto* p = processor())
                    p->setGain (fader.getValue() <= -59.9 ? 0.0f : juce::Decibels::decibelsToGain ((float) fader.getValue()));
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
            solo.setVisible (channelId != 0);
            solo.onClick = [this] { owner.engine.setAudioChannelSoloed (channelId, solo.getToggleState()); };
            mute.onClick = [this]
            {
                if (auto* p = processor())
                    p->setMuted (mute.getToggleState());
            };

            output.setTooltip ("Output selection - coming (phase 2); every channel goes to the master for now");
            output.setText (channelId != 0 ? juce::String::fromUTF8 ("\xe2\x86\x92 Master") : juce::String ("Device out"),
                            juce::dontSendNotification);
            output.setJustificationType (juce::Justification::centred);
            output.setFont (juce::FontOptions (10.5f));
            output.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
            output.setColour (juce::Label::outlineColourId, juce::Colours::white.withAlpha (0.12f));
            addAndMakeVisible (output);

            refreshName();
            syncControls();
        }

        AudioChannelProcessor* processor() const
        {
            return channelId == 0 ? owner.engine.getMasterChannel() : owner.engine.getAudioChannel (channelId);
        }

        void refreshName()
        {
            if (! name.isBeingEdited())
                name.setText (channelId == 0 ? juce::String ("Master") : owner.engine.getAudioChannelName (channelId),
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
                level.setText (fader.getValue() <= -59.9 ? juce::String (juce::CharPointer_UTF8 ("-\xe2\x88\x9e"))
                                                         : juce::String (fader.getValue(), 1),
                               juce::dontSendNotification);
            }

            if (channelId != 0)
                solo.setToggleState (owner.engine.isAudioChannelSoloed (channelId), juce::dontSendNotification);
        }

        void tick()
        {
            if (auto* p = processor())
                meter.update (p->getLastPeak(), p->getLastRms());
        }

        void paint (juce::Graphics& g) override
        {
            const auto isHighlighted = channelId != 0 && channelId == owner.highlighted;
            g.setColour (isHighlighted ? theme::colour (theme::Token::channelSelectedBg) : theme::colour (theme::Token::channelBg));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::corner);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (4);
            name.setBounds (area.removeFromTop (22));
            area.removeFromTop (4);
            inserts.setBounds (area.removeFromTop (78));
            area.removeFromTop (4);

            if (channelId != 0)
                sends.setBounds (area.removeFromTop (48));

            area.removeFromTop (4);
            output.setBounds (area.removeFromBottom (20));
            area.removeFromBottom (4);
            auto buttons = area.removeFromBottom (22);
            solo.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (1, 0));
            mute.setBounds (buttons.reduced (1, 0));
            area.removeFromBottom (4);
            level.setBounds (area.removeFromBottom (16));

            if (channelId != 0)
                pan.setBounds (area.removeFromTop (40).withSizeKeepingCentre (40, 40));

            auto faderArea = area.reduced (0, 2);
            meter.setBounds (faderArea.removeFromRight (12));
            faderArea.removeFromRight (4);
            fader.setBounds (faderArea);
        }

        MixerView& owner;
        const AudioEngine::AudioChannelId channelId;   // 0 = the master
        juce::Label name, level, output;
        Placeholder inserts { "Inserts", 4 }, sends { "Sends", 2 };
        juce::Slider pan, fader;
        Meter meter;
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
                auto strip = std::make_unique<Strip> (*this, id);
                strips.addAndMakeVisible (*strip);
                channelStrips.push_back (std::move (strip));
            }

            layoutStrips();
        }

        const auto revisionChanged = engine.getStateRevision() != lastRevision;
        lastRevision = engine.getStateRevision();

        for (auto& strip : channelStrips)
        {
            strip->tick();

            if (revisionChanged || ++syncCounter % 8 == 0)
            {
                strip->syncControls();
                strip->refreshName();
            }
        }

        master->tick();
        master->syncControls();
    }

    void layoutStrips()
    {
        const auto height = juce::jmax (300, viewport.getMaximumVisibleHeight());
        strips.setSize ((int) channelStrips.size() * (stripWidth + 4) + 4, height);

        for (size_t i = 0; i < channelStrips.size(); ++i)
            channelStrips[i]->setBounds (4 + (int) i * (stripWidth + 4), 4, stripWidth, height - 8);
    }

    AudioEngine& engine;
    juce::Label title;
    juce::Viewport viewport;
    juce::Component strips;
    std::vector<std::unique_ptr<Strip>> channelStrips;
    std::unique_ptr<Strip> master;
    std::vector<AudioEngine::AudioChannelId> shownIds;
    AudioEngine::AudioChannelId highlighted = 0;
    int lastRevision = -1, syncCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
