#pragma once

#include "../AudioEngine.h"

// The MIDI-domain sidebar: compact track rows with R/E/S/M/I buttons, grouped by
// folders (Cubase-style). Folder rows are half a track row tall, nest arbitrarily,
// and an indented guide area on the left shows what belongs to which folder.
// Rows are rebuilt when the engine's track/folder tree changes; call refresh()
// from a UI timer.
class TrackList final : public juce::Component
{
public:
    explicit TrackList (AudioEngine&);
    ~TrackList() override;

    void refresh();
    void setSelectedTrack (AudioEngine::TrackId);
    AudioEngine::TrackId getSelectedTrack() const noexcept { return selectedTrack; }

    std::function<void (AudioEngine::TrackId)> onSelect, onArm, onOpenEditor, onOpenInstrument, onShowContextMenu;
    std::function<void()> onAddTrack;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Row;
    class FolderRow;

    void rebuildRows();
    void layoutRows();
    void showFolderMenu (AudioEngine::FolderId);

    AudioEngine& engine;
    juce::TextButton addButton { "+ Track" }, addFolderButton { "+ Folder" };
    juce::Viewport viewport;
    juce::Component rowContainer;

    std::vector<AudioEngine::SidebarItem> items;          // what the rows were built from
    std::vector<std::unique_ptr<juce::Component>> rowComponents;   // parallel to items

    AudioEngine::TrackId selectedTrack = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackList)
};
