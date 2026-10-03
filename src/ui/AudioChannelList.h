#pragma once

#include "../AudioEngine.h"
#include "../engine/AudioChannelProcessor.h"

// The Audio-domain sidebar: one row per audio channel showing its input, a meter,
// mute and volume, grouped by folders (Cubase-style, like the track list): folder
// rows are half a channel row tall, nest arbitrarily, and the indented guide area
// on the left shows what belongs to which folder. Right-click a channel to move it
// into a folder. Call refresh() from a UI timer.
class AudioChannelList final : public juce::Component
{
public:
    explicit AudioChannelList (AudioEngine& e) : engine (e)
    {
        addFolderButton.setWantsKeyboardFocus (false);
        addFolderButton.setTooltip ("Add a folder for grouping channels (right-click channels and folders to move them)");
        addFolderButton.onClick = [this]
        {
            engine.addFolder (false);
            refresh();
        };
        addAndMakeVisible (addFolderButton);

        viewport.setViewedComponent (&rowContainer, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void refresh()
    {
        auto freshItems = engine.getSidebarItems (false, true);

        if (freshItems != items)
        {
            items = std::move (freshItems);
            rebuildRows();
        }

        for (auto& row : rowComponents)
        {
            if (auto* channelRow = dynamic_cast<Row*> (row.get()))
                channelRow->refresh();
            else if (auto* folderRow = dynamic_cast<FolderRow*> (row.get()))
                folderRow->refresh();
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        addFolderButton.setBounds (area.removeFromTop (26).reduced (6, 2));
        viewport.setBounds (area);
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232529));

        if (rowComponents.empty())
        {
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (13.0f));
            g.drawFittedText ("No audio channels yet.\nAdd an instrument and its channel appears here.",
                              getLocalBounds().reduced (12), juce::Justification::centredTop, 4);
        }
    }

private:
    static constexpr int rowHeight = 64;
    static constexpr int folderRowHeight = 32;   // about half a channel row
    static constexpr int indentPerLevel = 10;

    static void paintIndentGuides (juce::Graphics& g, int depth, int height)
    {
        for (int level = 1; level <= depth; ++level)
        {
            g.setColour (juce::Colours::gold.withAlpha (0.18f + 0.04f * (float) level));
            g.fillRect (level * indentPerLevel - 6, 0, 2, height);
        }
    }

    struct Row final : juce::Component
    {
        Row (AudioChannelList& ownerToUse, AudioEngine::AudioChannelId id, int depthToUse)
            : owner (ownerToUse), engine (ownerToUse.engine), channelId (id), depth (depthToUse)
        {
            muteButton.setClickingTogglesState (true);
            muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orange.darker (0.3f));
            muteButton.setWantsKeyboardFocus (false);
            muteButton.onClick = [this]
            {
                if (auto* processor = engine.getAudioChannel (channelId))
                    processor->setMuted (muteButton.getToggleState());
            };
            addAndMakeVisible (muteButton);

            volumeSlider.setRange (-60.0, 6.0, 0.1);
            volumeSlider.setValue (0.0, juce::dontSendNotification);
            volumeSlider.setDoubleClickReturnValue (true, 0.0);
            volumeSlider.setSkewFactorFromMidPoint (-12.0);
            volumeSlider.setWantsKeyboardFocus (false);
            volumeSlider.onValueChange = [this]
            {
                if (auto* processor = engine.getAudioChannel (channelId))
                    processor->setGain (juce::Decibels::decibelsToGain ((float) volumeSlider.getValue(), -60.0f));
            };
            addAndMakeVisible (volumeSlider);
        }

        void refresh()
        {
            if (auto* processor = engine.getAudioChannel (channelId))
            {
                muteButton.setToggleState (processor->isMuted(), juce::dontSendNotification);
                meterLevel = juce::jmax (processor->getLastPeak(), meterLevel * 0.85f);
            }

            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu())
                owner.showChannelMenu (channelId);
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
            g.setColour (juce::Colour (0xff2b2e33));
            g.fillRoundedRectangle (bounds, 4.0f);

            auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).reduced (8, 4);

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (engine.getAudioChannelName (channelId), area.removeFromTop (18), juce::Justification::centredLeft);

            const auto input = engine.getAudioChannelInput (channelId);
            g.setColour (juce::Colours::grey);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (input != 0 ? juce::String::fromUTF8 ("← ") + engine.getInstrumentName (input)
                                   : juce::String ("no input"),
                        area.removeFromTop (14), juce::Justification::centredLeft);

            // Horizontal meter under the controls
            auto meter = getLocalBounds().withTrimmedLeft (depth * indentPerLevel)
                             .reduced (8, 4).removeFromBottom (5).toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (meter);

            const auto db = juce::Decibels::gainToDecibels (meterLevel, -60.0f);
            const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f));
            g.setColour (meterLevel >= 1.0f ? juce::Colours::red : juce::Colours::limegreen);
            g.fillRect (meter.withWidth (meter.getWidth() * proportion));

            paintIndentGuides (g, depth, getHeight());
        }

        void resized() override
        {
            auto area = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).reduced (8, 4);
            area.removeFromTop (34);
            area.removeFromBottom (7);
            muteButton.setBounds (area.removeFromLeft (24));
            area.removeFromLeft (4);
            volumeSlider.setBounds (area);
        }

        AudioChannelList& owner;
        AudioEngine& engine;
        const AudioEngine::AudioChannelId channelId;
        const int depth;
        juce::TextButton muteButton { "M" };
        juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        float meterLevel = 0.0f;
    };

    struct FolderRow final : juce::Component
    {
        FolderRow (AudioChannelList& ownerToUse, AudioEngine::FolderId id, int depthToUse)
            : owner (ownerToUse), engine (ownerToUse.engine), folderId (id), depth (depthToUse)
        {
            nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
            nameLabel.setEditable (false, true);
            nameLabel.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            nameLabel.setColour (juce::Label::textColourId, juce::Colours::gold.withAlpha (0.85f));
            nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            nameLabel.onTextChange = [this]
            {
                engine.setFolderName (folderId, nameLabel.getText());
                nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);
            };
            nameLabel.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (nameLabel);
        }

        void refresh()
        {
            if (! nameLabel.isBeingEdited())
                nameLabel.setText (engine.getFolderName (folderId), juce::dontSendNotification);

            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu())
            {
                owner.showFolderMenu (folderId);
                return;
            }

            engine.setFolderCollapsed (folderId, ! engine.isFolderCollapsed (folderId));
            owner.refresh();
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            if (nameLabel.getBounds().contains (event.getPosition()))
                nameLabel.showEditor();
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().withTrimmedLeft (depth * indentPerLevel).toFloat().reduced (2.0f, 1.5f);
            g.setColour (juce::Colour (0xff2e3038));
            g.fillRoundedRectangle (bounds, 4.0f);

            const auto collapsed = engine.isFolderCollapsed (folderId);
            juce::Path triangle;
            const auto cx = bounds.getX() + 13.0f, cy = bounds.getCentreY();

            if (collapsed)
                triangle.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 5.0f, cy);
            else
                triangle.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 5.0f);

            g.setColour (juce::Colours::gold.withAlpha (0.8f));
            g.fillPath (triangle);

            paintIndentGuides (g, depth, getHeight());
        }

        void resized() override
        {
            nameLabel.setBounds (getLocalBounds().withTrimmedLeft (depth * indentPerLevel + 24).reduced (0, 2));
        }

        AudioChannelList& owner;
        AudioEngine& engine;
        const AudioEngine::FolderId folderId;
        const int depth;
        juce::Label nameLabel;
    };

    void rebuildRows()
    {
        rowComponents.clear();

        for (auto& item : items)
        {
            std::unique_ptr<juce::Component> row;

            if (item.folder != 0)
                row = std::make_unique<FolderRow> (*this, item.folder, item.depth);
            else
                row = std::make_unique<Row> (*this, item.member, item.depth);

            rowContainer.addAndMakeVisible (*row);
            rowComponents.push_back (std::move (row));
        }

        layoutRows();
        repaint();
    }

    void layoutRows()
    {
        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        int y = 0;

        for (size_t i = 0; i < rowComponents.size(); ++i)
        {
            const auto height = items[i].folder != 0 ? folderRowHeight : rowHeight;
            rowComponents[i]->setBounds (0, y, width, height);
            y += height;
        }

        rowContainer.setSize (width, juce::jmax (1, y));
    }

    void showChannelMenu (AudioEngine::AudioChannelId channelId)
    {
        const auto safe = juce::Component::SafePointer<AudioChannelList> (this);
        juce::PopupMenu moveTo;

        moveTo.addItem ("Top level", true, engine.getAudioChannelFolder (channelId) == 0,
                        [safe, channelId] { if (safe != nullptr) { safe->engine.setAudioChannelFolder (channelId, 0); safe->refresh(); } });

        for (auto folderId : engine.getFolderIds (false))
            moveTo.addItem (engine.getFolderName (folderId), true,
                            engine.getAudioChannelFolder (channelId) == folderId,
                            [safe, channelId, folderId]
                            { if (safe != nullptr) { safe->engine.setAudioChannelFolder (channelId, folderId); safe->refresh(); } });

        moveTo.addSeparator();
        moveTo.addItem ("New folder", [safe, channelId]
        {
            if (safe != nullptr)
            {
                safe->engine.setAudioChannelFolder (channelId, safe->engine.addFolder (false));
                safe->refresh();
            }
        });

        juce::PopupMenu menu;
        menu.addSubMenu ("Move to folder", moveTo);
        menu.showMenuAsync (juce::PopupMenu::Options());
    }

    void showFolderMenu (AudioEngine::FolderId folderId)
    {
        const auto safe = juce::Component::SafePointer<AudioChannelList> (this);
        juce::PopupMenu menu;

        menu.addItem ("New subfolder", [safe, folderId]
        {
            if (safe != nullptr)
            {
                safe->engine.addFolder (false, {}, folderId);
                safe->engine.setFolderCollapsed (folderId, false);
                safe->refresh();
            }
        });

        juce::PopupMenu moveTo;
        moveTo.addItem ("Top level", true, engine.getFolderParent (folderId) == 0, [safe, folderId]
        {
            if (safe != nullptr) { safe->engine.setFolderParent (folderId, 0); safe->refresh(); }
        });

        for (auto id : engine.getFolderIds (false))
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

    AudioEngine& engine;
    juce::TextButton addFolderButton { "+ Folder" };
    juce::Viewport viewport;
    juce::Component rowContainer;

    std::vector<AudioEngine::SidebarItem> items;
    std::vector<std::unique_ptr<juce::Component>> rowComponents;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioChannelList)
};
