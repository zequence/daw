#pragma once

#include "../AudioEngine.h"

// The MIDI-domain sidebar: compact track rows with R/E/S/M/I buttons.
// Rows are rebuilt when the engine's track set changes; call refresh() from a UI timer.
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

    void rebuildRows();
    void layoutRows();

    AudioEngine& engine;
    juce::TextButton addButton { "+ Track" };
    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<Row>> rows;
    AudioEngine::TrackId selectedTrack = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackList)
};
