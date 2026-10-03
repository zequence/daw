#pragma once

#include "../AudioEngine.h"

// The MIDI-domain sidebar: compact track rows with R/E/S/M/I buttons, grouped by
// folders (Cubase-style). Folder rows are half a track row tall, nest arbitrarily,
// and an indented guide area on the left shows what belongs to which folder.
//
// Re-ordering (ISSUES.md "Sidebar"): drag rows to re-order; Ctrl/Shift-click
// selects multiple tracks and dragging any selected row moves the group (it lands
// in visual order). The drag starts once the mouse leaves the pressed row and
// completes on drop: between rows inserts there, onto a folder row's middle drops
// into that folder. Rows rebuild when the engine's tree changes; call refresh()
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
    std::function<void (AudioEngine::FolderId)> onAddTrackInFolder;   // folder menu "New track inside"

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Row;
    class FolderRow;

    struct RowContainer final : juce::Component
    {
        explicit RowContainer (TrackList& o) : owner (o) {}
        void paintOverChildren (juce::Graphics&) override;   // drop indicator
        void mouseDown (const juce::MouseEvent&) override;   // background context menu
        TrackList& owner;
    };

    void rebuildRows();
    void layoutRows();
    void refreshSoon();                   // deferred refresh, safe from row callbacks
    void showFolderMenu (AudioEngine::FolderId);
    void showBackgroundMenu();            // right-click on the empty area

    static int heightOfItem (const AudioEngine::SidebarItem&);

    // Selection + drag (called by the rows)
    void rowMouseDown (juce::Component* row, bool isFolder, int id, const juce::MouseEvent&);
    void rowMouseDrag (juce::Component* row, bool isFolder, int id, const juce::MouseEvent&);
    bool finishRowDrag (int id);          // true if a drag was in progress (even an invalid one)
    void computeDropTarget (int yInContainer);
    std::vector<AudioEngine::TrackId> selectionInVisualOrder() const;

    AudioEngine& engine;
    juce::TextButton addButton { "+ Track" }, addFolderButton { "+ Folder" };
    juce::Viewport viewport;
    RowContainer rowContainer { *this };

    std::vector<AudioEngine::SidebarItem> items;          // what the rows were built from
    std::vector<std::unique_ptr<juce::Component>> rowComponents;   // parallel to items

    AudioEngine::TrackId selectedTrack = 0;

    // Multi-select (UI-level; the primary selection stays with MainComponent)
    std::set<AudioEngine::TrackId> multiSelection;
    AudioEngine::TrackId shiftAnchor = 0;
    bool clearSelectionOnMouseUp = false;

    struct DragState
    {
        bool active = false;              // true once the mouse left the pressed row
        bool sourceIsFolder = false;
        int sourceId = 0;
        std::vector<AudioEngine::TrackId> draggedTracks;

        bool valid = false, intoFolder = false;
        AudioEngine::FolderId parent = 0;
        int index = 0;
        int indicatorY = -1;              // container coords
        juce::Rectangle<int> folderHighlight;
    } drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackList)
};
