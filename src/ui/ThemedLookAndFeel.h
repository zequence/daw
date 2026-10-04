#pragma once

#include "Theme.h"

// The app's LookAndFeel: stock JUCE (V4 dark) with its colors driven by the
// theme, so every standard widget follows the Buttons tokens. Buttons with a
// special look (transport, arm/solo/mute, toggles, top bar) carry a "role"
// (setButtonRole) that picks their own tokens; everything else uses button.*.
// Colors are looked up while drawing, so a theme change only needs a repaint.
namespace theme
{
    inline void setButtonRole (juce::Component& button, const char* role)
    {
        button.getProperties().set ("themeRole", role);
    }
}

class ThemedLookAndFeel final : public juce::LookAndFeel_V4,
                                private juce::ChangeListener
{
public:
    ThemedLookAndFeel()
    {
        theme::Manager::get().addChangeListener (this);
        applyTheme();
    }

    ~ThemedLookAndFeel() override   { theme::Manager::get().removeChangeListener (this); }

    // Standard TextButtons pass findColour (buttonColourId / buttonOnColourId);
    // roles replace that color with their own token.
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        using T = theme::Token;
        const auto role = button.getProperties()["themeRole"].toString();
        const auto on = button.getToggleState();
        auto colour = backgroundColour;

        const auto pick = [&] (T off, T onToken) { colour = theme::colour (on ? onToken : off); };

        if (role == "rtz")            colour = theme::colour (T::transportRtzBg);
        else if (role == "play")      pick (T::transportPlayBg, T::transportPlayOn);
        else if (role == "record")    pick (T::transportRecordBg, T::transportRecordOn);
        else if (role == "loop")      pick (T::transportLoopBg, T::transportLoopOn);
        else if (role == "arm")       pick (T::buttonBg, T::trackArmOn);
        else if (role == "solo")      pick (T::buttonBg, T::trackSoloOn);
        else if (role == "mute")      pick (T::buttonBg, T::trackMuteOn);
        else if (role == "accent")    pick (T::buttonBg, T::buttonAccentOn);
        else if (role == "topbar")    pick (T::topbarButtonBg, T::topbarButtonOn);

        juce::LookAndFeel_V4::drawButtonBackground (g, button, colour, highlighted, down);
    }

private:
    // The widgets without roles take their colors from the LookAndFeel's table
    void applyTheme()
    {
        using T = theme::Token;
        const auto bg = theme::colour (T::buttonBg), on = theme::colour (T::buttonOn),
                   border = theme::colour (T::buttonBorder), text = theme::colour (T::buttonText);

        setColour (juce::TextButton::buttonColourId, bg);
        setColour (juce::TextButton::buttonOnColourId, on);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, text);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, text);
        setColour (juce::ComboBox::backgroundColourId, bg);
        setColour (juce::ComboBox::outlineColourId, border);   // also the outline of buttons
        setColour (juce::ComboBox::buttonColourId, border);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, text);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        applyTheme();   // repainting is done by the theme editor's change handler
    }
};
