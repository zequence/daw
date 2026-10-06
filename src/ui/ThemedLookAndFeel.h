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

        // Stock shading (hover brightens, press darkens), but barely rounded corners;
        // edges joined to a neighbour stay square
        auto base = colour.withMultipliedSaturation (button.hasKeyboardFocus (true) ? 1.3f : 0.9f)
                          .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

        if (down || highlighted)
            base = base.contrasting (down ? 0.2f : 0.05f);

        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto left = button.isConnectedOnLeft(), right = button.isConnectedOnRight(),
                   top = button.isConnectedOnTop(), bottom = button.isConnectedOnBottom();
        juce::Path path;
        path.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                                  theme::corner, theme::corner,
                                  ! (left || top), ! (right || top), ! (left || bottom), ! (right || bottom));

        g.setColour (base);
        g.fillPath (path);
        g.setColour (button.findColour (juce::ComboBox::outlineColourId));
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (bounds, theme::corner);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (bounds, theme::corner, 1.0f);

        // The arrow, as stock
        const auto arrowZone = juce::Rectangle<int> (width - 30, 0, 20, height).toFloat();
        juce::Path arrow;
        arrow.startNewSubPath (arrowZone.getX() + 3.0f, arrowZone.getCentreY() - 2.0f);
        arrow.lineTo (arrowZone.getCentreX(), arrowZone.getCentreY() + 3.0f);
        arrow.lineTo (arrowZone.getRight() - 3.0f, arrowZone.getCentreY() - 2.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 0.9f : 0.2f));
        g.strokePath (arrow, juce::PathStrokeType (2.0f));
    }

    // Flat menu: the background and a hairline border
    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
        g.setColour (theme::colour (theme::Token::menuBorder));
        g.drawRect (0, 0, width, height, 1);
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

        // Popup menus (the hamburger menu, right-click menus, combo box lists)
        setColour (juce::PopupMenu::backgroundColourId, theme::colour (T::menuBg));
        setColour (juce::PopupMenu::textColourId, theme::colour (T::menuText));
        setColour (juce::PopupMenu::headerTextColourId, theme::colour (T::menuHeaderText));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::colour (T::menuHighlightBg));
        setColour (juce::PopupMenu::highlightedTextColourId, theme::colour (T::menuHighlightText));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        applyTheme();   // repainting is done by the theme editor's change handler
    }
};
