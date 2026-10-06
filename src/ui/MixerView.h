#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"
#include "MixerParts.h"

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
        addMouseListener (this, true);   // a right-click anywhere: the mixer's menu (the console style)

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

    // The right-click menu: the console style (SSL for now; more styles to come)
    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! event.mods.isPopupMenu())
            return;

        juce::PopupMenu styles;
        styles.addItem (mixer::ConsoleStyle::ssl().name, true, true, [] {});
        juce::PopupMenu menu;
        menu.addSubMenu ("Console style", styles);
        menu.showMenuAsync (juce::PopupMenu::Options());
    }

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
        outer.setBounds (area);
        layoutBody();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceContent));
    }

    // Studio lights: a soft, wide sheen falling diagonally over the whole console
    // (drawn once per size into an image - the meters repaint often, and the image is cheaper to lay on)
    void paintOverChildren (juce::Graphics& g) override
    {
        if (sheenImage.getWidth() != getWidth() || sheenImage.getHeight() != getHeight())
        {
            sheenImage = juce::Image (juce::Image::ARGB, juce::jmax (1, getWidth()), juce::jmax (1, getHeight()), true);
            juce::Graphics ig (sheenImage);
            const auto w = (float) getWidth(), h = (float) getHeight();
            juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.0f), 0.0f, 0.0f,
                                        juce::Colours::white.withAlpha (0.0f), w, h, false);
            sheen.addColour (0.30, juce::Colours::white.withAlpha (0.05f));
            sheen.addColour (0.42, juce::Colours::white.withAlpha (0.09f));
            sheen.addColour (0.55, juce::Colours::white.withAlpha (0.03f));
            sheen.addColour (0.80, juce::Colours::black.withAlpha (0.06f));
            ig.setGradientFill (sheen);
            ig.fillAll();
        }

        g.drawImageAt (sheenImage, 0, 0);
    }

    juce::Image sheenImage;

private:
    enum class Kind { channel, aux, master };
    static constexpr int stripWidth = 136, stripHeight = 1420;

    const mixer::ConsoleStyle& style() const   { return mixer::ConsoleStyle::ssl(); }

    //==========================================================================
    // A section of a strip: a panel with a legend; its controls are laid out by the strip. The panel
    // takes the shape the strip gives it (a section can end in one column while the next begins in
    // the other: their edges then run diagonally); without a shape it fills its bounds.
    struct Section final : juce::Component
    {
        explicit Section (const juce::String& legendToUse) : legend (legendToUse) {}

        juce::Path outline() const
        {
            if (! shape.isEmpty())
                return shape;

            juce::Path box;
            box.addRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
            return box;
        }

        void paint (juce::Graphics& g) override
        {
            const auto& style = mixer::ConsoleStyle::ssl();
            const auto path = outline();
            g.setColour (style.section);
            g.fillPath (path);

            {
                juce::Graphics::ScopedSaveState state (g);   // recessed: a shade under the top edges
                g.reduceClipRegion (path);
                g.setColour (juce::Colours::black.withAlpha (0.16f));
                g.strokePath (path, juce::PathStrokeType (5.0f), juce::AffineTransform::translation (0.0f, 2.5f));
            }

            g.setColour (style.sectionLine);
            g.strokePath (path, juce::PathStrokeType (1.0f));
            g.setColour (style.sectionText);
            g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
            g.drawText (legend, titleArea.isEmpty() ? getLocalBounds().removeFromTop (12) : titleArea, juce::Justification::centred, false);
        }

        bool hitTest (int x, int y) override   { return outline().contains ((float) x, (float) y); }

        juce::String legend;
        juce::Path shape;
        juce::Rectangle<int> titleArea;
    };

    //==========================================================================
    struct Strip final : juce::Component
    {
        Strip (MixerView& o, Kind kindToUse, AudioEngine::AudioChannelId id, int auxNumberToUse)
            : owner (o), kind (kindToUse), channelId (id), auxNumber (auxNumberToUse)
        {
            // The strip (with its knobs) is kept as an image: scrolling only moves pictures, and a
            // turned knob or a moving meter redraws just its own patch of it
            setBufferedToImage (true);

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
            inserts.slots = kind == Kind::aux ? 8 : 16;   // channels and the master 16 inserts, Aux buses 8
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

                eqIn.led = dynamicsIn.led = true;   // the sections' on/off: LEDs
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
            // The background is drawn once into an image, again only when the size or highlight changes
            const auto isHighlighted = kind == Kind::channel && channelId == owner.highlighted;

            if (background.getWidth() != getWidth() || background.getHeight() != getHeight() || isHighlighted != backgroundHighlighted)
            {
                backgroundHighlighted = isHighlighted;
                background = makeBackground (isHighlighted);
            }

            g.drawImageAt (background, 0, 0);
        }

        juce::Image makeBackground (bool isHighlighted) const
        {
            const auto w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
            juce::Image image (juce::Image::ARGB, w, h, true);
            juce::Graphics g (image);
            juce::Path panel;
            panel.addRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
            g.setColour (isHighlighted ? owner.style().panel.brighter (0.25f) : owner.style().panel);
            g.fillPath (panel);

            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (panel);
                g.drawImageAt (grain (w, h, 1234 + (juce::int64) channelId * 7919 + auxNumber * 104729 + (int) kind * 31), 0, 0);
            }

            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.fillRect (0, 2, 1, h - 4);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (w - 1, 2, 1, h - 4);

            for (auto p : { juce::Point<float> (5.0f, 5.0f), { (float) w - 5.0f, 5.0f }, { 5.0f, (float) h - 5.0f }, { (float) w - 5.0f, (float) h - 5.0f } })
            {
                const auto screw = juce::Rectangle<float> (5.0f, 5.0f).withCentre (p);
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xffb8bcc2), screw.getX(), screw.getY(),
                                                         juce::Colour (0xff4a4e54), screw.getRight(), screw.getBottom(), false));
                g.fillEllipse (screw);
                g.setColour (juce::Colours::black.withAlpha (0.6f));
                g.drawEllipse (screw, 0.6f);
                g.drawLine (p.x - 1.8f, p.y + 1.0f, p.x + 1.8f, p.y - 1.0f, 0.9f);
            }

            if (kind != Kind::channel)   // Aux and master: a coloured band under the name
            {
                g.setColour (kind == Kind::master ? juce::Colour (0xffc23b33) : owner.style().auxCap);
                g.fillRect (getLocalBounds().reduced (4, 0).withTop (27).withHeight (2));
            }

            return image;
        }

        // A strip's grain, made once per seed (kept while the app runs; strips rebuilt or resized reuse
        // it), tall enough for any strip so far - in steps of 512 px
        static juce::Image grain (int w, int h, juce::int64 seed)
        {
            static std::map<std::pair<juce::int64, int>, juce::Image> made;
            auto& image = made[{ seed, w }];

            if (image.getHeight() < h)
                image = makeTexture (w, (h + 511) / 512 * 512, seed);

            return image;
        }

        // Brushed metal: fine vertical grain. Each strip gets its own seed, so no two are brushed alike.
        static juce::Image makeTexture (int w, int h, juce::int64 seed)
        {
            juce::Image image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
            juce::Random random (seed);

            // Slow, uneven patches of contrast across the strip: a few soft waves with random phases
            struct Wave { float fx, fy, phase, amount; };
            std::array<Wave, 4> waves;

            for (auto& wave : waves)
                wave = { 0.02f + random.nextFloat() * 0.06f, 0.01f + random.nextFloat() * 0.03f,
                         random.nextFloat() * juce::MathConstants<float>::twoPi, 0.08f + random.nextFloat() * 0.12f };

            {
                juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);

                for (int x = 1; x < w - 1; ++x)
                {
                    // A grain line is broken into streaks of random length, each a little lighter or darker
                    auto streak = 0.0f;
                    auto streakEnd = 0;
                    const auto lineBias = random.nextFloat() * 2.0f - 1.0f;

                    for (int y = 0; y < h; ++y)
                    {
                        if (y >= streakEnd)
                        {
                            streak = 0.3f * lineBias + 0.7f * (random.nextFloat() * 2.0f - 1.0f);
                            streakEnd = y + 10 + random.nextInt (90);
                        }

                        auto contrast = 1.0f;

                        for (const auto& wave : waves)
                            contrast += wave.amount * std::sin (wave.fx * (float) x + wave.fy * (float) y + wave.phase);

                        const auto noise = random.nextFloat() * 2.0f - 1.0f;
                        const auto v = juce::jlimit (-1.0f, 1.0f, (streak * 0.85f + noise * 0.12f) * juce::jmax (0.2f, contrast));
                        const auto alpha = std::abs (v) * (v > 0.0f ? 0.032f : 0.018f);   // the dark streaks lighter than the bright ones
                        pixels.setPixelColour (x, y, (v > 0.0f ? juce::Colours::white : juce::Colours::black).withAlpha (alpha));
                    }
                }
            }

            return image;
        }

        juce::Image background;
        bool backgroundHighlighted = false;

        void resized() override
        {
            auto area = getLocalBounds().reduced (4);
            name.setBounds (area.removeFromTop (22));
            area.removeFromTop (4);

            if (kind == Kind::master)
            {
                inserts.setBounds (area.removeFromTop (330));   // its 16 slots
                area.removeFromTop (5);
            }
            else
            {
                flip.setBounds (area.removeFromTop (16).withSizeKeepingCentre (64, 15));
                area.removeFromTop (4);
            }

            // The knobs, staggered: two columns that overlap a little, each knob diagonally below the
            // last one in the other column, so they pack tightly. A section that ends in one column
            // lets the next begin beside it; their panels follow the knobs, diagonal where they meet.
            if (kind != Kind::master)
            {
                constexpr int knobW = 68, knobH = 75, stagger = 39, buttonW = 44, buttonH = 16, titleH = 12, pad = 2, gap = 4;
                const auto left = area.getX() + pad, right = area.getRight() - pad, mid = area.getCentreX();
                const int columnX[2] = { left, right - knobW };
                int columnY[2] = { area.getY() + pad, area.getY() + pad };
                int lastY = area.getY() - stagger, next = 0;

                struct Extent { int top = std::numeric_limits<int>::max(), bottom = std::numeric_limits<int>::min(); };
                std::array<Extent, 2> extent;
                std::vector<std::pair<juce::Component*, juce::Rectangle<int>>> placed;
                Section* current = nullptr;
                juce::Rectangle<int> title;

                const auto mark = [&] (int column, int top, int bottom)
                {
                    extent[(size_t) column].top = juce::jmin (extent[(size_t) column].top, top);
                    extent[(size_t) column].bottom = juce::jmax (extent[(size_t) column].bottom, bottom);
                };

                const auto finish = [&]
                {
                    if (current == nullptr)
                        return;

                    for (int c = 0; c < 2; ++c)   // a column without controls follows the other
                        if (extent[(size_t) c].top > extent[(size_t) c].bottom)
                            extent[(size_t) c] = extent[(size_t) (1 - c)];

                    const auto x0 = (float) (left - pad), x1 = (float) (right + pad), m = (float) mid, d = 10.0f;
                    const auto t0 = (float) (extent[0].top - pad), t1 = (float) (extent[1].top - pad);
                    const auto b0 = (float) (extent[0].bottom + pad), b1 = (float) (extent[1].bottom + pad);
                    juce::Path path;
                    path.startNewSubPath (x0, t0);
                    path.lineTo (m - d, t0);
                    path.lineTo (m + d, t1);
                    path.lineTo (x1, t1);
                    path.lineTo (x1, b1);
                    path.lineTo (m + d, b1);
                    path.lineTo (m - d, b0);
                    path.lineTo (x0, b0);
                    path.closeSubPath();

                    const auto bounds = path.getBounds().getSmallestIntegerContainer();
                    current->setBounds (bounds);
                    current->shape = path.createPathWithRoundedCorners (3.0f);
                    current->shape.applyTransform (juce::AffineTransform::translation ((float) -bounds.getX(), (float) -bounds.getY()));
                    current->titleArea = title - bounds.getPosition();

                    for (auto& [component, r] : placed)
                        component->setBounds (r - bounds.getPosition());

                    placed.clear();
                    extent = {};
                    current = nullptr;
                };

                const auto begin = [&] (Section& section, juce::Component* onOff)
                {
                    const auto first = current == nullptr;
                    finish();
                    current = &section;

                    if (! first)
                        for (auto& y : columnY)
                            y += gap + 2 * pad;

                    // The title, at the top of the column the section's first knob goes into
                    title = { columnX[next], columnY[next], knobW, titleH };
                    mark (next, title.getY(), title.getBottom());
                    columnY[next] = title.getBottom();

                    if (onOff != nullptr)   // the section's on/off, at the top beside the first knob
                    {
                        const auto c = 1 - next;
                        const auto y = juce::jmax (columnY[c], title.getY());
                        placed.push_back ({ onOff, juce::Rectangle<int> (columnX[c], y, knobW, buttonH).withSizeKeepingCentre (buttonW, buttonH - 2) });
                        mark (c, y, y + buttonH);
                        columnY[c] = y + buttonH;
                    }
                };

                const auto knob = [&] (juce::Component* k)
                {
                    const auto y = juce::jmax (columnY[next], lastY + stagger);
                    placed.push_back ({ k, { columnX[next], y, knobW, knobH } });
                    mark (next, y, y + knobH);
                    columnY[next] = y + knobH;
                    lastY = y;
                    next = 1 - next;
                };

                const auto button = [&] (juce::Component& b)   // in the column with more room
                {
                    const auto c = columnY[0] <= columnY[1] ? 0 : 1;
                    placed.push_back ({ &b, juce::Rectangle<int> (columnX[c], columnY[c], knobW, buttonH).withSizeKeepingCentre (buttonW, buttonH - 2) });
                    mark (c, columnY[c], columnY[c] + buttonH);
                    columnY[c] += buttonH;
                };

                const auto pageTop = area.getY();

                begin (eq, &eqIn);
                for (auto* k : { &hpf, &lpf, &hfGain, &hfFreq })
                    knob (k);
                button (hfBell);
                for (auto* k : { &hmfGain, &hmfFreq, &hmfQ, &lmfGain, &lmfFreq, &lmfQ, &lfGain, &lfFreq })
                    knob (k);
                button (lfBell);

                begin (dynamics, &dynamicsIn);
                for (auto* k : { &threshold, &ratio, &attack, &release, &makeup })
                    knob (k);

                auto pageBottom = juce::jmax (columnY[0], columnY[1]) + pad;   // the inserts share the EQ and dynamics' space

                if (kind == Kind::channel)
                {
                    begin (aux, nullptr);

                    for (auto& k : auxKnobs)
                        knob (k.get());
                }

                finish();

                if (kind == Kind::channel)
                    pageBottom = aux.getY() - 2;

                inserts.setBounds (getLocalBounds().reduced (4).withTop (pageTop).withBottom (pageBottom));
                area.setTop (juce::jmax (columnY[0], columnY[1]) + pad + 5);
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
                auto knobs = area.removeFromTop (48);   // the big knobs keep their size
                drive.setBounds (knobs.removeFromLeft (knobs.getWidth() / 2));
                pan.setBounds (knobs);
                area.removeFromTop (4);
            }

            auto faderArea = area.reduced (0, 2);   // the level runs to the bottom
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
    juce::Viewport outer, channelsViewport;
    juce::Component body, strips;
    std::vector<std::unique_ptr<Strip>> channelStrips, auxStrips;
    std::unique_ptr<Strip> master;
    std::vector<AudioEngine::AudioChannelId> shownIds;
    AudioEngine::AudioChannelId highlighted = 0;
    int lastRevision = -1, syncCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerView)
};
