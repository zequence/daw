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

        if (auto* slots = dynamic_cast<mixer::Placeholder*> (event.eventComponent); slots != nullptr && slots->onSlotClicked != nullptr)
            return;   // the inserts have their own menu

        juce::PopupMenu styles;
        styles.addItem (mixer::ConsoleStyle::ssl().name, true, true, [] {});
        juce::PopupMenu menu;
        const auto safe = juce::Component::SafePointer<MixerView> (this);
        menu.addItem ("Fit size (to the window's height)", [safe] { if (safe != nullptr) safe->fitSize(); });
        menu.addItem ("Actual size (100%)", [safe]
        {
            if (safe == nullptr)
                return;

            safe->scale = 1.0f;
            safe->engine.getSettingsFile().setValue ("mixerZoom", 1.0);
            safe->applyScale();
        });
        menu.addSeparator();
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
            {
                strip->repaint();

                if (strip->channelId == id)   // brought into view (selected in the sidebar's list)
                {
                    const auto x = strip->getX();
                    const auto visible = channelsViewport.getViewArea();

                    if (x < visible.getX() || strip->getRight() > visible.getRight())
                        channelsViewport.setViewPosition (juce::jmax (0, x - 4), visible.getY());
                }
            }
        }
    }

    // A click anywhere on a channel's strip (a knob too) selects that channel - here and in the
    // sidebar's audio channel list
    std::function<void (AudioEngine::AudioChannelId)> onChannelSelected;

    // The mixer keeps one size (the strips never stretch, so resizing the window only moves the view's
    // edge). Its zoom changes on command only: "Fit size" (the right-click menu) zooms it to the
    // window's height; the zoom is kept in the settings.
    void resized() override
    {
        if (scale <= 0.0f)
            scale = juce::jlimit (0.4f, 2.0f, (float) engine.getSettingsFile().getDoubleValue ("mixerZoom", 1.0));

        applyScale();

        if (rack != nullptr)
            rack->setBounds (rackShown ? getLocalBounds() : getLocalBounds().translated (20000, 0));   // else beyond any window edge
    }

    void fitSize()
    {
        scale = juce::jlimit (0.4f, 2.0f, (float) getHeight() / (float) juce::jmax (1, body.getHeight()));
        engine.getSettingsFile().setValue ("mixerZoom", (double) scale);
        engine.getSettingsFile().saveIfNeeded();
        applyScale();
    }

    void applyScale()
    {
        outer.setTransform (juce::AffineTransform::scale (scale));
        outer.setBounds (0, 0, juce::roundToInt (std::ceil ((float) getWidth() / scale)), juce::roundToInt (std::ceil ((float) getHeight() / scale)));
        layoutBody();
    }

    int levelTop = 0;

public:
    // An insert's window (MainComponent owns the plugin windows); the hook before an insert goes,
    // so its window closes before the plugin is deleted
    std::function<void (AudioEngine::AudioChannelId, int slot)> onOpenInsert, onBeforeInsertRemove;

private:   // where a channel strip's pan and fader begin (the other strips follow it)
    float scale = 0.0f;   // 0 = not yet read from the settings

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour (theme::Token::surfaceContent));
    }

    // Studio lights: a soft, wide sheen falling diagonally over the whole console
    // (drawn once per size into an image - the meters repaint often, and the image is cheaper to lay on)
    void paintOverChildren (juce::Graphics& g) override
    {
        if (rack != nullptr && rackShown)   // the rack is not under the console's lights
            return;

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
    static constexpr int stripWidth = 118, stripHeight = 1300;

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
            addMouseListener (this, true);   // its controls' clicks select the channel too (mouseDown)

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
            inserts.slots = kind == Kind::aux ? 8 : 16;
            if (kind == Kind::channel)
            {
                inserts.setTooltip ("Click an empty slot to add an effect, a full one to open it (right-click: bypass, remove)");
                inserts.onSlotClicked = [this] (int slot, const juce::MouseEvent& event) { insertClicked (slot, event); };
            }

            insertsIn.led = true;
            insertsIn.setToggleState (true, juce::dontSendNotification);
            if (kind == Kind::channel)
            {
                insertsIn.setTooltip ("All the inserts on / off (each keeps its own bypass)");
                insertsIn.onClick = [this] { owner.engine.setInsertsEnabled (channelId, insertsIn.getToggleState()); };
            }
            else
            {
                insertsIn.setTooltip ("All the inserts on / off - " + placeholderTip);
            }
            // In the INSERTS flip button, beside its text (the master, with no flip: in the inserts' box)
            if (kind == Kind::master)
                inserts.addAndMakeVisible (insertsIn);
            else
                flip.addAndMakeVisible (insertsIn);   // channels and the master 16 inserts, Aux buses 8
            addChildComponent (inserts);

            // The inserts sit behind the EQ and dynamics: this flips between them
            flip.setTooltip ("Show the inserts (in place of the EQ and dynamics) - click again for the EQ");
            // A channel's INSERTS button opens its rack (in the rack: closes it); the Aux buses flip in place
            flip.onClick = [this]
            {
                if (kind != Kind::channel)
                {
                    showInserts (flip.getToggleState());
                    return;
                }

                flip.setToggleState (inRack, juce::dontSendNotification);
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MixerView> (&owner), id = channelId, closing = inRack]
                {
                    if (safe != nullptr)
                        closing ? safe->closeRack() : safe->openRack (id);
                });
            };

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
            dynamics.setVisible (kind != Kind::master);   // the inserts take the EQ's place only
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

        // The inserts (channels): the effects menu by category on an empty slot, the effect's window on a
        // full one; right-click a full one for its menu (removing is a menu item, never the click itself)
        void insertClicked (int slot, const juce::MouseEvent& event)
        {
            auto& engine = owner.engine;
            const auto inserts = engine.getInserts (channelId);
            const auto it = std::find_if (inserts.begin(), inserts.end(), [slot] (const auto& i) { return i.slot == slot; });
            const auto safe = juce::Component::SafePointer<Strip> (this);
            const auto id = channelId;

            if (it != inserts.end() && ! event.mods.isPopupMenu())
            {
                if (owner.onOpenInsert) owner.onOpenInsert (id, slot);
                return;
            }

            juce::PopupMenu menu;

            if (it != inserts.end())
            {
                menu.addItem ("Open " + it->name, [safe, id, slot] { if (safe != nullptr && safe->owner.onOpenInsert) safe->owner.onOpenInsert (id, slot); });
                menu.addItem ("Bypass", true, it->bypassed, [safe, id, slot, bypassed = it->bypassed]
                              { if (safe != nullptr) safe->owner.engine.setInsertBypassed (id, slot, ! bypassed); });
                menu.addSeparator();
                juce::PopupMenu replace;
                addEffectsMenu (replace, slot);
                menu.addSubMenu ("Replace with", replace);
                menu.addItem ("Remove", [safe, id, slot]
                {
                    if (safe == nullptr)
                        return;

                    safe->owner.beforeInsertRemove (id, slot);
                    safe->owner.engine.removeInsert (id, slot);
                });
            }
            else
            {
                addEffectsMenu (menu, slot);
            }

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&this->inserts));
        }

        // The effects by category (VST3's subcategories, e.g. "Fx|EQ" -> EQ), each a submenu
        void addEffectsMenu (juce::PopupMenu& menu, int slot)
        {
            std::map<juce::String, juce::PopupMenu> categories;
            const auto safe = juce::Component::SafePointer<Strip> (this);
            const auto id = channelId;

            for (const auto& type : owner.engine.getEffectTypes())
            {
                auto parts = juce::StringArray::fromTokens (type.category, "|", {});
                parts.removeString ("Fx");
                parts.removeEmptyStrings();
                const auto category = parts.isEmpty() ? juce::String ("Other") : parts[0];
                const auto label = type.name + (type.manufacturerName.isNotEmpty() ? "  (" + type.manufacturerName + ")" : juce::String());

                categories[category].addItem (label, [safe, id, slot, type]
                {
                    if (safe == nullptr)
                        return;

                    safe->owner.beforeInsertRemove (id, slot);   // a replaced effect's editor and window
                    safe->owner.engine.addInsert (id, slot, type);
                });
            }

            if (categories.empty())
                menu.addItem ("No effects found - scan for plugins in Settings", false, false, [] {});

            for (auto& [name, submenu] : categories)
                menu.addSubMenu (name, submenu);
        }

        void syncInserts()
        {
            if (kind != Kind::channel)
                return;

            std::vector<juce::String> names ((size_t) inserts.slots);
            std::vector<bool> bypassed ((size_t) inserts.slots);

            for (const auto& insert : owner.engine.getInserts (channelId))
                if (insert.slot < inserts.slots)
                {
                    names[(size_t) insert.slot] = insert.name;
                    bypassed[(size_t) insert.slot] = insert.bypassed;
                }

            insertsIn.setToggleState (owner.engine.areInsertsEnabled (channelId), juce::dontSendNotification);

            if (names != inserts.names || bypassed != inserts.bypassed)
            {
                inserts.names = std::move (names);
                inserts.bypassed = std::move (bypassed);
                inserts.repaint();
            }
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            if (kind == Kind::channel)
            {
                owner.setHighlightedChannel (channelId);

                if (owner.onChannelSelected)
                    owner.onChannelSelected (channelId);
            }
        }

        void tick()
        {
            level.setVisible (fader.isMouseButtonDown());   // the fader's dB shows only while it moves

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
                flip.setBounds (area.removeFromTop (16).withSizeKeepingCentre (80, 15));
                area.removeFromTop (4);
            }

            // The knobs, staggered: two columns that overlap a little, each knob diagonally below the
            // last one in the other column, so they pack tightly. A section that ends in one column
            // lets the next begin beside it; their panels follow the knobs, diagonal where they meet.
            if (kind != Kind::master)
            {
                constexpr int knobW = 58, knobH = 64, stagger = 33, buttonW = 44, buttonH = 16, titleH = 12, pad = 2, gap = 1;
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
                        const auto y = juce::jmax (columnY[c], title.getY()) + 5;   // a little down from the panel's top
                        auto r = juce::Rectangle<int> (columnX[c], y, knobW, buttonH).withSizeKeepingCentre (buttonW, buttonH - 2);
                        placed.push_back ({ onOff, c == 1 ? r.withX (right - buttonW) : r.withX (left) });   // out towards the edge
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


                if (kind == Kind::channel)
                {
                    begin (aux, nullptr);

                    // The sends: the EQ and dynamics' knobs, on their grid (the same zigzag, the same y)
                    for (auto& k : auxKnobs)
                        knob (k.get());
                }

                finish();

                // The inserts, flipped in, in the EQ's place: their slots the master's size (330 px for 16)
                inserts.setBounds (getLocalBounds().reduced (4).withTop (pageTop).withHeight (330 * inserts.slots / 16));
                area.setTop (juce::jmax (columnY[0], columnY[1]) + pad + 2);
            }

            if (kind == Kind::master)
                insertsIn.setBounds (42, 0, 16, 13);   // beside the inserts' caption
            else
                insertsIn.setBounds (2, 0, 16, flip.getHeight());
            output.setBounds (area.removeFromBottom (20));
            area.removeFromBottom (4);
            level.setBounds (area.removeFromBottom (16));

            // The faders are one height on every strip: the Aux buses and the master (no aux sends,
            // no EQ) start theirs where the channels' do - the master's, below where pan would be
            if (kind == Kind::channel)
                owner.levelTop = area.getY();
            else if (owner.levelTop > 0)
                area.setTop (owner.levelTop + (kind == Kind::master ? 58 + 4 : 0));

            if (kind != Kind::master)   // drive and pan, side by side, by the level
            {
                auto knobs = area.removeFromTop (58);   // pan as big as the EQ's knobs, drive 46 px
                // Centred on the knob columns above (the EQ's, the sends'), so their middles line up
                const auto leftMiddle = knobs.getX() + 2 + 58 / 2, rightMiddle = knobs.getRight() - 2 - 58 / 2;
                // Pan's centre level with drive's (drive's legend takes 7 px under its 46 px knob)
                pan.setBounds (juce::Rectangle<int> (58, 58).withCentre ({ rightMiddle, 0 }).withY (knobs.getY()));
                drive.setBounds (juce::Rectangle<int> (48, 55).withCentre ({ leftMiddle, 0 }).withY (knobs.getY() + 29 - 24));
                area.removeFromTop (4);
            }

            auto faderArea = area.reduced (0, 2);   // the level runs to the bottom
            meter.setBounds (faderArea.removeFromRight (12));
            faderArea.removeFromRight (4);

            // Solo and mute beside the fader, at its foot, one above the other
            auto buttons = faderArea.removeFromLeft (24);
            mute.setBounds (buttons.removeFromBottom (22));
            buttons.removeFromBottom (4);
            solo.setBounds (buttons.removeFromBottom (22));
            faderArea.removeFromLeft (2);
            fader.setBounds (faderArea);
        }

        bool inRack = false;   // the strip beside the rack
        MixerView& owner;
        const Kind kind;
        const AudioEngine::AudioChannelId channelId;   // channels
        const int auxNumber;                           // Aux buses: 1-6
        juce::Label name, level, output;
        mixer::Placeholder inserts { "Inserts", 8 };
        mixer::LitButton flip { "INSERTS", juce::Colour (0xff9fb3c8) };
        mixer::LitButton insertsIn { {}, juce::Colour (0xff62d26f) };   // the whole insert section on/off
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
    // The rack: one channel's inserts as rack units, every plugin's own editor open in its unit at
    // its own size, top to bottom in slot order (a plugin that resizes itself re-flows the rack).
    // Empty slots are blank panels (click: the effects menu). The channel's strip stands at the left.
    // The editors are native windows, so the rack is never zoomed; the mixer's zoom stays outside it.
    struct Rack final : juce::Component, private juce::ComponentListener
    {
        Rack (MixerView& o, AudioEngine::AudioChannelId id) : owner (o), channelId (id)
        {
            strip = std::make_unique<Strip> (o, Kind::channel, id, 0);
            strip->inRack = true;
            strip->flip.setToggleState (true, juce::dontSendNotification);
            strip->setSize (stripWidth, stripHeight);
            stripView.setViewedComponent (strip.get(), false);
            stripView.setScrollBarsShown (true, false);
            addAndMakeVisible (stripView);

            setOpaque (true);   // the mixer underneath is not painted while the rack covers it
            column.rack = this;
            view.setViewedComponent (&column, false);
            addAndMakeVisible (view);
            // (the editors are made by sync(), once the rack has been placed out of sight)
        }

        ~Rack() override
        {
            for (auto& unit : units)
                dropEditor (unit);
        }

        static constexpr int headerHeight = 22, emptyHeight = 24, ear = 18, gap = 6, minWidth = 420;

        struct Unit
        {
            int slot = 0;
            juce::String name;
            bool bypassed = false;
            juce::AudioPluginInstance* plugin = nullptr;
            std::unique_ptr<juce::AudioProcessorEditor> editor;
            juce::Rectangle<int> panel, header, led;
        };

        // The drawn rack: rails at the sides, a faceplate per unit, blank panels for empty slots
        struct Column final : juce::Component
        {
            void paint (juce::Graphics& g) override
            {
                g.fillAll (theme::colour (theme::Token::surfaceContent));   // beside the rack (it scrolls there too)
                const auto w = rack->rackWidth;
                g.setColour (juce::Colour (0xff141619));   // the rack's inside
                g.fillRect (0, 0, w, getHeight());

                for (auto x : { 0, w - ear })   // the rails: black, with their holes
                {
                    g.setColour (juce::Colour (0xff0b0b0c));
                    g.fillRect (x, 0, ear, getHeight());
                    g.setColour (juce::Colour (0xff2a2c30));

                    for (int y = 6; y < getHeight(); y += 15)
                        g.fillRoundedRectangle ((float) x + (float) ear * 0.5f - 3.0f, (float) y, 6.0f, 8.0f, 2.0f);
                }

                for (auto& unit : rack->units)
                {
                    const auto panel = unit.panel.toFloat();
                    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4f56), 0.0f, panel.getY(),
                                                             juce::Colour (0xff33373c), 0.0f, panel.getBottom(), false));
                    g.fillRect (panel);
                    g.setColour (juce::Colours::black.withAlpha (0.7f));
                    g.drawRect (panel, 1.0f);

                    for (auto p : { juce::Point<float> (panel.getX() + 7.0f, panel.getY() + 7.0f), { panel.getRight() - 7.0f, panel.getY() + 7.0f } })
                    {
                        g.setColour (juce::Colour (0xffa9adb3));
                        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (p));
                        g.setColour (juce::Colours::black.withAlpha (0.6f));
                        g.drawLine (p.x - 1.8f, p.y + 1.0f, p.x + 1.8f, p.y - 1.0f, 0.9f);
                    }

                    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                    const auto text = unit.header.reduced (18, 0);

                    if (unit.plugin == nullptr)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.3f));
                        g.drawText (juce::String (unit.slot + 1) + "   empty - click to add an effect", text, juce::Justification::centredLeft, false);
                        continue;
                    }

                    // The bypass LED: lit while the effect is in
                    const auto lamp = unit.led.toFloat().withSizeKeepingCentre (9.0f, 9.0f);
                    g.setColour (juce::Colour (0xff0c0d0f));
                    g.fillEllipse (lamp.expanded (1.5f));
                    g.setColour (unit.bypassed ? juce::Colour (0xff2a3a2c) : juce::Colour (0xff62d26f));
                    g.fillEllipse (lamp);

                    g.setColour (juce::Colours::white.withAlpha (unit.bypassed ? 0.45f : 0.9f));
                    g.drawText (juce::String (unit.slot + 1) + "   " + unit.name, text.withTrimmedLeft (18), juce::Justification::centredLeft, false);
                    g.setColour (juce::Colours::white.withAlpha (0.5f));
                    g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), text, juce::Justification::centredRight, false);
                }
            }

            void mouseDown (const juce::MouseEvent& event) override
            {
                for (auto& unit : rack->units)
                    if (unit.header.contains (event.getPosition()) || (unit.plugin == nullptr && unit.panel.contains (event.getPosition())))
                    {
                        rack->clicked (unit, event);
                        return;
                    }
            }

            Rack* rack = nullptr;
        };

        void clicked (Unit& unit, const juce::MouseEvent& event)
        {
            const auto slot = unit.slot;
            const auto id = channelId;
            auto& engine = owner.engine;

            if (unit.plugin != nullptr && unit.led.expanded (4).contains (event.getPosition()) && ! event.mods.isPopupMenu())
            {
                engine.setInsertBypassed (id, slot, ! unit.bypassed);
                return;
            }

            juce::PopupMenu menu;

            if (unit.plugin != nullptr)
            {
                const auto safe = juce::Component::SafePointer<MixerView> (&owner);
                menu.addItem ("Bypass", true, unit.bypassed, [&engine, id, slot, bypassed = unit.bypassed] { engine.setInsertBypassed (id, slot, ! bypassed); });
                menu.addSeparator();
                juce::PopupMenu replace;
                strip->addEffectsMenu (replace, slot);
                menu.addSubMenu ("Replace with", replace);
                menu.addItem ("Remove", [safe, id, slot]
                {
                    if (safe == nullptr)
                        return;

                    safe->beforeInsertRemove (id, slot);
                    safe->engine.removeInsert (id, slot);
                });
            }
            else
            {
                strip->addEffectsMenu (menu, slot);
            }

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&column));
        }

        // The editor goes before its plugin does (removed or replaced)
        void dropEditor (Unit& unit)
        {
            if (unit.editor != nullptr)
            {
                unit.editor->removeComponentListener (this);
                unit.editor.reset();   // (its destructor tells the plugin)
            }
        }

        void dropEditor (int slot)
        {
            for (auto& unit : units)
                if (unit.slot == slot)
                    dropEditor (unit);
        }

        // Follows the engine: new effects get their editor, changed names and bypass repaint
        void sync()
        {
            const auto inserts = owner.engine.getInserts (channelId);
            auto changed = units.size() != (size_t) AudioEngine::insertSlots;
            units.resize ((size_t) AudioEngine::insertSlots);

            for (int slot = 0; slot < AudioEngine::insertSlots; ++slot)
            {
                auto& unit = units[(size_t) slot];
                unit.slot = slot;
                const auto it = std::find_if (inserts.begin(), inserts.end(), [slot] (const auto& i) { return i.slot == slot; });
                auto* plugin = it != inserts.end() ? owner.engine.getInsertPlugin (channelId, slot) : nullptr;

                if (plugin != unit.plugin)
                {
                    dropEditor (unit);   // (its plugin went through beforeInsertRemove, which dropped it already)
                    unit.plugin = plugin;

                    if (plugin != nullptr)
                    {
                        auto* editor = plugin->hasEditor() ? plugin->createEditorAndMakeActive() : nullptr;

                        if (editor == nullptr)
                            editor = new juce::GenericAudioProcessorEditor (*plugin);

                        unit.editor.reset (editor);
                        column.addAndMakeVisible (editor);
                        owner.rackEditorsOpenedAt = juce::Time::getMillisecondCounter();   // see MainComponent's timer
                        editor->addComponentListener (this);
                    }

                    changed = true;
                }

                const auto name = it != inserts.end() ? it->name : juce::String();
                const auto bypassed = it != inserts.end() && it->bypassed;

                if (name != unit.name || bypassed != unit.bypassed)
                {
                    unit.name = name;
                    unit.bypassed = bypassed;
                    column.repaint();
                }
            }

            if (changed)
                layoutUnits();
        }

        // As wide as the widest editor; each editor centred in its unit, at its own size
        void layoutUnits()
        {
            auto inner = minWidth;

            for (auto& unit : units)
                if (unit.editor != nullptr)
                    inner = juce::jmax (inner, unit.editor->getWidth() + 8);

            const auto width = inner + 2 * ear;
            auto y = gap;

            for (auto& unit : units)
            {
                const auto editorHeight = unit.editor != nullptr ? unit.editor->getHeight() + 4 : 0;
                unit.panel = { ear, y, inner, unit.editor != nullptr ? headerHeight + editorHeight : emptyHeight };
                unit.header = unit.panel.withHeight (unit.editor != nullptr ? headerHeight : emptyHeight);
                unit.led = unit.header.withWidth (14).translated (20, 0);

                if (unit.editor != nullptr)
                    unit.editor->setTopLeftPosition (ear + (inner - unit.editor->getWidth()) / 2, y + headerHeight);   // never resized by us

                y = unit.panel.getBottom() + (unit.editor != nullptr ? gap : 2);
            }

            rackWidth = width;   // the column fills the view, so the wheel scrolls beside the rack too
            column.setSize (juce::jmax (width, view.getMaximumVisibleWidth()), juce::jmax (y + gap, view.getMaximumVisibleHeight()));
            lastChange = juce::Time::getMillisecondCounter();   // the rack shows once this settles (see revealRackWhenSettled)
            column.repaint();
        }

        void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
        {
            if (wasResized)   // a plugin resized itself (its own size option)
                layoutUnits();
        }

        void resized() override
        {
            auto area = getLocalBounds();
            stripView.setBounds (area.removeFromLeft (stripWidth + stripView.getScrollBarThickness() + 8).reduced (4, 0));
            view.setBounds (area);
            layoutUnits();
        }

        void paint (juce::Graphics& g) override   { g.fillAll (theme::colour (theme::Token::surfaceContent)); }

        void tick()
        {
            strip->tick();
            strip->syncControls();
            strip->syncInserts();
            sync();
        }

        MixerView& owner;
        const AudioEngine::AudioChannelId channelId;
        std::unique_ptr<Strip> strip;
        juce::Viewport stripView, view;
        Column column;
        std::vector<Unit> units;
        int rackWidth = 0;
        juce::uint32 lastChange = 0;
    };

    std::unique_ptr<Rack> rack;
    std::map<AudioEngine::AudioChannelId, std::pair<juce::Point<int>, juce::Point<int>>> rackScroll;   // each channel's rack and strip scroll (this session)

public:
    // The rack for a channel (its strip's INSERTS button): the other strips step aside
    void openRack (AudioEngine::AudioChannelId id)
    {
        if (onBeforeInsertRemove)
            onBeforeInsertRemove (id, -1);   // its inserts' windows close: the rack shows their editors

        if (rackPending)
            return;

        // The last rack is kept (out of sight) after it closes: the same channel's comes back at once,
        // its plugins already drawn; another channel's replaces it
        if (rack != nullptr && rack->channelId == id)
        {
            rackShown = true;
            resized();
            return;
        }

        destroyRack();

        // The rack is built out of sight (far beyond the window - its plugin editors are native windows,
        // so nothing can be drawn over them while they load); the mixer stays until it has settled

        rackPending = true;
        engine.getBusyStatus().begin ("Opening the rack");   // the busy box, until it shows
        engine.getBusyStatus().update ("Loading the inserts' editors");

        // The editors load (and block) only once the busy box has been drawn
        juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<MixerView> (this), id]
        {
            if (safe == nullptr || ! safe->rackPending || safe->rack != nullptr)
                return;   // closed meanwhile

            safe->rackOpenedAt = juce::Time::getMillisecondCounter();
            safe->rack = std::make_unique<Rack> (*safe, id);
            safe->addAndMakeVisible (*safe->rack);
            safe->resized();          // far outside the window first: the editors' native windows can't show
            safe->rack->sync();       // then the editors load

            if (auto it = safe->rackScroll.find (id); it != safe->rackScroll.end())   // where it was left
            {
                safe->rack->view.setViewPosition (it->second.first);
                safe->rack->stripView.setViewPosition (it->second.second);
            }
        });
    }

    // Called by the timer: the rack shows once its editors have stopped loading and resizing
    void revealRackWhenSettled()
    {
        if (rack == nullptr || ! rackPending)
            return;

        const auto now = juce::Time::getMillisecondCounter();

        if (now - rack->lastChange > 500 || now - rackOpenedAt > 5000)
        {
            rackPending = false;
            rackShown = true;
            engine.getBusyStatus().end();
            resized();   // over the mixer, which stays as it is underneath (closing the rack needs no redraw)
        }
    }

    // Back to the mixer: the rack goes out of sight but stays (see openRack)
    void closeRack()
    {
        if (rackPending)   // closed while still loading
        {
            destroyRack();
            return;
        }

        if (rack != nullptr)
            rackScroll[rack->channelId] = { rack->view.getViewPosition(), rack->stripView.getViewPosition() };

        rackShown = false;
        resized();
    }

    // The rack and its editors go for good - before a project load, an instrument's removal, etc.
    void destroyRack()
    {
        if (rackPending)
            engine.getBusyStatus().end();

        if (rack != nullptr && rackShown)
            rackScroll[rack->channelId] = { rack->view.getViewPosition(), rack->stripView.getViewPosition() };

        rackPending = rackShown = false;
        rack.reset();
        resized();
    }

    bool isRackOpen() const   { return rack != nullptr && rackShown; }
    bool rackShown = false;                 // in view (else kept out of sight, or none)
    juce::uint32 rackEditorsOpenedAt = 0;
    bool rackPending = false;               // built, loading out of sight
    juce::uint32 rackOpenedAt = 0;   // when the rack last created plugin editors (they may take the window's activation)

    // Before an insert goes (removed or replaced): its editor in the rack and its window close first
    void beforeInsertRemove (AudioEngine::AudioChannelId id, int slot)
    {
        if (rack != nullptr && rack->channelId == id)
            rack->dropEditor (slot);

        if (onBeforeInsertRemove)
            onBeforeInsertRemove (id, slot);
    }

private:

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
                strip->syncInserts();
            }
        }

        master->tick();

        if (rack != nullptr)
        {
            rack->tick();
            revealRackWhenSettled();
        }
        master->syncControls();
    }

    // The row: channels and Aux buses scrolling sideways, the master fixed at the right; the
    // whole row scrolls up and down when the view is shorter than a strip
    void layoutBody()
    {
        // The strips never change size (the zoom fits them to the window); room is kept under them
        // for the channels' scrollbar only when it shows
        const auto width = outer.getMaximumVisibleWidth();
        const auto gap = 14;   // between the channels and the Aux buses
        const auto count = (int) channelStrips.size();
        const auto stripsWidth = count * (stripWidth + 4) + gap + 6 * (stripWidth + 4) + 4;
        const auto viewWidth = width - stripWidth - 20;
        const auto scrollbar = stripsWidth > viewWidth ? channelsViewport.getScrollBarThickness() : 0;
        const auto stripsHeight = stripHeight + 8;
        const auto height = stripsHeight + scrollbar;
        body.setSize (width, height);

        master->setBounds (width - stripWidth - 8, 4, stripWidth, stripHeight);
        channelsViewport.setBounds (4, 0, viewWidth, height);
        strips.setSize (stripsWidth, stripsHeight);

        for (int i = 0; i < count; ++i)
            channelStrips[(size_t) i]->setBounds (4 + i * (stripWidth + 4), 4, stripWidth, stripsHeight - 8);

        for (int i = 0; i < (int) auxStrips.size(); ++i)
            auxStrips[(size_t) i]->setBounds (4 + count * (stripWidth + 4) + gap + i * (stripWidth + 4), 4, stripWidth, stripsHeight - 8);

        for (auto& strip : auxStrips)   // now that the channels have set levelTop
            strip->resized();

        master->resized();
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
