#pragma once

#include "ThemedLookAndFeel.h"

// The mixer's drawn parts (MixerView.h): the console style, knobs, lit buttons, the level fader,
// the meter and placeholders. Everything is drawn in code - sharp at any scale, no image licences.
namespace mixer
{
//==============================================================================
// A console style: the strip's colours and its knob caps. SSL first; more styles later.
struct ConsoleStyle
{
    juce::String name;
    juce::Colour panel, section, sectionText, sectionLine;
    juce::Colour filterCap, hfCap, hmfCap, lmfCap, lfCap, dynamicsCap, auxCap, panCap, driveCap;
    juce::Colour eqLit, dynamicsLit, bellLit;

    static const ConsoleStyle& ssl()
    {
        static const ConsoleStyle style {
            "SSL",
            juce::Colour (0xff3b4046),   // the strip: SSL's dark grey
            juce::Colour (0xff31353a),   // a section's panel
            juce::Colour (0xffd8d8d2),   // printed legends
            juce::Colour (0xff4a4f56),
            juce::Colour (0xff7a5a3e),   // filters: brown
            juce::Colour (0xffc23b33),   // HF: red
            juce::Colour (0xff3f9a4c),   // HMF: green
            juce::Colour (0xff3a6fbf),   // LMF: blue
            juce::Colour (0xff2a2a2c),   // LF: black
            juce::Colour (0xffd8d4c8),   // dynamics: light grey
            juce::Colour (0xffe0b43a),   // aux: yellow
            juce::Colour (0xff6b6f75),   // pan: grey
            juce::Colour (0xffd9772e),   // drive: orange
            juce::Colour (0xff62d26f),   // EQ in: green light
            juce::Colour (0xffeec34a),   // dynamics in: yellow light
            juce::Colour (0xffe0564c)    // bell: red light
        };

        return style;
    }
};

//==============================================================================
// A small knob: a coloured cap with a white pointer, an arc showing the setting (from the centre
// for a bipolar knob), a legend underneath. While it is turned its value shows on the cap.
struct Knob : juce::Slider
{
    Knob (const juce::String& legendToUse, juce::Colour capToUse, bool bipolarToUse)
        : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox),
          legend (legendToUse), cap (capToUse), bipolar (bipolarToUse)
    {
        setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    }

    std::function<juce::String (double)> format = [] (double v) { return juce::String (v, 1); };
    bool alwaysShowValue = false;   // the pan knob: its value always shows (dimmed)

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto legendArea = legend.isNotEmpty() ? area.removeFromBottom (11.0f) : juce::Rectangle<float>();
        const auto size = juce::jmin (area.getWidth(), area.getHeight()) - 2.0f;
        const auto circle = juce::Rectangle<float> (size, size).withCentre (area.getCentre());
        const auto centre = circle.getCentre();
        const auto radius = size * 0.5f;
        const auto params = getRotaryParameters();
        const auto angle = params.startAngleRadians
                             + (float) valueToProportionOfLength (getValue()) * (params.endAngleRadians - params.startAngleRadians);
        const auto from = bipolar ? (params.startAngleRadians + params.endAngleRadians) * 0.5f : params.startAngleRadians;

        // The printed scale on the panel: 11 ticks, the ends (and a bipolar knob's centre) longer
        for (int i = 0; i <= 10; ++i)
        {
            const auto a = params.startAngleRadians + (float) i / 10.0f * (params.endAngleRadians - params.startAngleRadians);
            const auto major = i == 0 || i == 10 || (bipolar && i == 5);
            g.setColour (juce::Colours::white.withAlpha (major ? 0.7f : 0.38f));
            g.drawLine ({ centre.getPointOnCircumference (radius - (major ? 4.5f : 3.2f), a),
                          centre.getPointOnCircumference (radius - 0.3f, a) }, major ? 1.3f : 1.0f);
        }

        const auto arcRadius = radius - 7.0f;
        juce::Path track, arc;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, params.startAngleRadians, params.endAngleRadians, true);
        g.setColour (juce::Colour (0xff141619));
        g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (std::abs (angle - from) > 0.01f)
        {
            arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (cap.brighter (0.4f).withSaturation (juce::jmin (1.0f, cap.getSaturation() + 0.1f)));
            g.strokePath (arc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // The knob, like a real one: a soft shadow on the panel, a dark ridged skirt (it turns with
        // the knob), and the coloured cap on top - domed, lit from above, with a shine
        const auto skirt = circle.reduced (10.5f);
        const auto body = skirt.reduced (skirt.getWidth() * 0.16f);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (skirt.translated (0.8f, 1.8f).expanded (0.6f));

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4d52), skirt.getCentreX(), skirt.getY(),
                                                 juce::Colour (0xff141518), skirt.getCentreX(), skirt.getBottom(), false));
        g.fillEllipse (skirt);

        const auto skirtRadius = skirt.getWidth() * 0.5f, capRadius = body.getWidth() * 0.5f;

        for (int i = 0; i < 28; ++i)   // the knurling
        {
            const auto a = angle + (float) i * juce::MathConstants<float>::twoPi / 28.0f;
            const auto lightSide = std::cos (a) < 0.0f;   // ridges catch the light on top
            g.setColour (lightSide ? juce::Colours::white.withAlpha (0.16f) : juce::Colours::black.withAlpha (0.35f));
            g.drawLine ({ centre.getPointOnCircumference (capRadius + 0.6f, a), centre.getPointOnCircumference (skirtRadius - 0.6f, a) }, 1.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (skirt, 1.0f);

        juce::Path bevel;   // the skirt's rim catching the light from above
        bevel.addCentredArc (centre.x, centre.y, skirtRadius - 1.2f, skirtRadius - 1.2f, 0.0f,
                             -juce::MathConstants<float>::pi * 0.4f, juce::MathConstants<float>::pi * 0.4f, true);
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.strokePath (bevel, juce::PathStrokeType (1.0f));

        juce::ColourGradient dome (cap.brighter (0.5f), body.getCentreX() - capRadius * 0.3f, body.getY() + capRadius * 0.25f,
                                   cap.darker (0.55f), body.getCentreX() + capRadius * 0.4f, body.getBottom(), true);
        dome.addColour (0.45, cap);
        g.setGradientFill (dome);
        g.fillEllipse (body);

        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.38f), body.getCentreX(), body.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), body.getCentreX(), body.getCentreY(), false));
        g.fillEllipse (body.reduced (body.getWidth() * 0.12f, 0.0f).withHeight (body.getHeight() * 0.48f).translated (0.0f, 1.0f));

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawEllipse (body, 0.8f);

        // A machined ring on the cap's top: a dark groove with a light edge under it
        const auto ring = body.reduced (body.getWidth() * 0.2f);
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.drawEllipse (ring, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.drawEllipse (ring.translated (0.0f, 0.8f), 0.8f);

        // The pointer: white with a dark outline, from the rim inwards
        const auto inner = skirt.getWidth() * 0.5f;
        juce::Path pointer;
        pointer.startNewSubPath (centre.getPointOnCircumference (capRadius * 0.3f, angle));
        pointer.lineTo (centre.getPointOnCircumference (inner - 0.5f, angle));
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.strokePath (pointer, juce::PathStrokeType (3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white);
        g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (alwaysShowValue || isMouseButtonDown())
        {
            g.setColour (juce::Colours::white.withAlpha (isMouseButtonDown() ? 0.95f : 0.4f));
            g.setFont (juce::FontOptions (alwaysShowValue ? 10.5f : 10.0f, juce::Font::bold));
            g.drawText (format (getValue()), skirt.expanded (6.0f).toNearestInt(), juce::Justification::centred, false);
        }

        if (! legendArea.isEmpty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.65f));
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawText (legend, legendArea.toNearestInt(), juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& event) override   { juce::Slider::mouseUp (event); repaint(); }

    juce::String legend;
    juce::Colour cap;
    bool bipolar;
};

//==============================================================================
// A small on/off button: dark when off, lit in its colour when on
struct LitButton : juce::Button
{
    LitButton (const juce::String& text, juce::Colour litToUse) : juce::Button (text), lit (litToUse)
    {
        setClickingTogglesState (true);
        setWantsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto on = getToggleState();
        g.setColour (on ? lit : juce::Colour (0xff1c1e22).brighter (highlighted ? 0.15f : 0.0f));
        g.fillRoundedRectangle (area, 2.0f);

        if (on)   // a glow
        {
            g.setColour (lit.withAlpha (0.35f));
            g.drawRoundedRectangle (area.expanded (0.5f), 2.5f, 1.5f);
        }

        g.setColour (on ? juce::Colours::black.withAlpha (0.85f) : juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

    juce::Colour lit;
};

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
// The level fader, in the style of a 70s/80s console: a recessed slot, a printed dB scale,
// and a chunky brushed-metal cap with grip ridges and a white index line at the level
struct LevelFader final : juce::Slider
{
    LevelFader() : juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox) {}

    static constexpr float capHeight = 34.0f, capWidth = 26.0f;

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto slotX = bounds.getRight() - capWidth * 0.5f - 2.0f;   // the slot sits right, the scale left
        const auto top = (float) getPositionOfValue (getMaximum());
        const auto bottom = (float) getPositionOfValue (getMinimum());

        // The scale: ticks and numbers, 0 dB brighter
        g.setFont (juce::FontOptions (8.5f));

        for (auto [db, text] : std::initializer_list<std::pair<double, const char*>> {
                 { 6.0, "+6" }, { 0.0, "0" }, { -5.0, "5" }, { -10.0, "10" }, { -20.0, "20" },
                 { -30.0, "30" }, { -40.0, "40" }, { -60.0, "-" } })
        {
            const auto y = (float) getPositionOfValue (db);
            const auto zero = db == 0.0;
            g.setColour (juce::Colours::white.withAlpha (zero ? 0.85f : 0.45f));
            g.fillRect (slotX - capWidth * 0.5f - 5.0f, y - 0.5f, zero ? 6.0f : 4.0f, 1.0f);
            g.drawText (db <= -60.0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e")) : juce::String (text),
                        juce::Rectangle<float> (0.0f, y - 6.0f, slotX - capWidth * 0.5f - 6.0f, 12.0f),
                        juce::Justification::centredRight, false);
        }

        // The slot: dark and recessed
        const auto slot = juce::Rectangle<float> (slotX - 2.5f, top, 5.0f, bottom - top);
        g.setColour (juce::Colour (0xff0b0c0e));
        g.fillRoundedRectangle (slot, 2.5f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (slot.translated (0.0f, 0.5f), 2.5f, 1.0f);

        // The cap, centred on the level
        const auto y = (float) getPositionOfValue (getValue());
        const auto cap = juce::Rectangle<float> (capWidth, capHeight).withCentre ({ slotX, y });

        g.setColour (juce::Colours::black.withAlpha (0.45f));   // its shadow on the panel
        g.fillRoundedRectangle (cap.translated (1.5f, 2.5f), 3.0f);

        juce::ColourGradient metal (juce::Colour (0xffd9dcdf), cap.getX(), cap.getY(),
                                    juce::Colour (0xff7d8186), cap.getX(), cap.getBottom(), false);
        metal.addColour (0.48, juce::Colour (0xffb7bbbf));
        metal.addColour (0.52, juce::Colour (0xff9a9ea3));
        g.setGradientFill (metal);
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);

        // Grip ridges above and below the index line
        for (int i = 1; i <= 4; ++i)
            for (auto sign : { -1.0f, 1.0f })
            {
                const auto ridgeY = y + sign * (3.0f + (float) i * 3.2f);
                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.fillRect (cap.getX() + 3.0f, ridgeY, cap.getWidth() - 6.0f, 1.0f);
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.fillRect (cap.getX() + 3.0f, ridgeY + 1.0f, cap.getWidth() - 6.0f, 0.8f);
            }

        // The white index line: exactly the level
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (cap.getX() + 1.0f, y - 1.5f, cap.getWidth() - 2.0f, 3.0f);
        g.setColour (juce::Colours::white);
        g.fillRect (cap.getX() + 1.0f, y - 0.75f, cap.getWidth() - 2.0f, 1.5f);
    }
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

}   // namespace mixer
