#pragma once

#include "../AudioEngine.h"
#include "SidebarMetrics.h"

// The MIDI-domain sidebar: compact track rows with R/E/S/M/I buttons, grouped by
// folders (Cubase-style). Folder rows are half a track row tall, nest arbitrarily,
// and an indented guide area on the left shows what belongs to which folder.
//
// Re-ordering (ISSUES.md "Sidebar"): drag rows to re-order; Ctrl/Shift-click
// selects several rows of any kind (folders, instruments, MIDI tracks, audio tracks)
// and dragging any selected row moves them all (they land in visual order; what's in a
// selected folder goes with it). An instrument's own tracks dragged alone reorder in it. The drag starts once the mouse leaves the pressed row and
// completes on drop: between rows inserts there, onto a folder row's middle drops
// into that folder. Rows rebuild when the engine's tree changes; call refresh()
// from a UI timer.
class TrackList final : public juce::Component
{
public:
    TrackList (AudioEngine&, sidebar::VerticalScroll&);   // scroll shared with the arrangement
    ~TrackList() override;

    void refresh();
    void rowHeightsChanged()   { rebuildRows(); }   // the track height zoom
    std::function<void (int direction)> onTrackHeightZoom;   // Ctrl+Shift+wheel over the list
    void setSelectedTrack (AudioEngine::TrackId);
    AudioEngine::TrackId getSelectedTrack() const noexcept { return selectedTrack; }
    const std::set<AudioEngine::TrackId>& getMultiSelection() const noexcept { return multiSelection; }

    // The tracks shown in the MIDI editor (a bar on their right edge; the edited one's wider). Empty = none.
    void setEditedTracks (std::vector<AudioEngine::TrackId> shown, AudioEngine::TrackId edited);
    std::vector<AudioEngine::TrackId> editorTracks;
    AudioEngine::TrackId editorTrack = 0;
    AudioEngine::FolderId getSelectedFolder() const noexcept   { return selectedFolders.size() == 1 ? *selectedFolders.begin() : 0; }
    AudioEngine::InstrumentId getSelectedInstrument() const noexcept   { return selectedInstruments.size() == 1 ? *selectedInstruments.begin() : 0; }
    AudioEngine::AudioChannelId getSelectedChannel() const noexcept { return selectedChannels.empty() ? 0 : *selectedChannels.begin(); }
    const std::set<AudioEngine::AudioChannelId>& getSelectedChannels() const noexcept { return selectedChannels; }
    void selectChannels (const std::set<AudioEngine::AudioChannelId>&);   // from the mixer
    void renameInstrument (AudioEngine::InstrumentId);                     // its row's name, edited in place
    std::function<void()> onGroupSelected;   // a folder, an instrument or an audio row chosen (the regions' selection goes)
    std::function<void (std::vector<AudioEngine::TrackId>)> onOpenEditorOnTracks;   // an instrument folder double-clicked
    std::function<void (AudioEngine::InstrumentId)> onInstrumentMenu;                 // right-click on an instrument folder or its audio
    std::function<void (AudioEngine::FolderId)> onRemoveFolder;                       // the folder and everything in it (asks first)

    std::function<void (AudioEngine::TrackId)> onSelect, onArm, onShowContextMenu;

    // The multi-selection changed (Ctrl/Shift clicks, folder selection clears it).
    // Receives every selected track; empty = just the primary selection.
    std::function<void (const std::set<AudioEngine::TrackId>&)> onSelectionChanged;
    std::function<void()> onAddTrack;
    std::function<void (AudioEngine::FolderId)> onAddTrackInFolder;   // folder menu "New track inside"

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Row;
    class FolderRow;
    class InstrumentRow;
    class AudioRow;

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
    void selectFolder (AudioEngine::FolderId);   // highlights the folder (its tracks stay unselected)
    void selectInstrument (AudioEngine::InstrumentId);
    void selectChannel (AudioEngine::AudioChannelId, juce::ModifierKeys = {});   // an audio row (Ctrl toggles, Shift a range)
    void setSubtreeCollapsed (AudioEngine::FolderId, bool collapsed);   // folder + all subfolders
    void showFolderMenu (AudioEngine::FolderId);
    void showBackgroundMenu();            // right-click on the empty area

    static int heightOfItem (const AudioEngine::SidebarItem&);

    // Selection + drag (called by the rows). A row: what it stands for (an instrument's output
    // channel is selectable, not movable)
    struct RowRef
    {
        enum class Kind { folder, track, audioTrack, instrument, channel };
        Kind kind = Kind::track;
        int id = 0;
    };

    RowRef refOf (const AudioEngine::SidebarItem&) const;
    bool isSelected (RowRef) const;
    int selectionCount() const;
    void selectOnly (RowRef);                             // a plain click: that one alone
    void rowMouseDown (juce::Component* row, RowRef, const juce::MouseEvent&);
    void rowMouseDrag (juce::Component* row, RowRef, const juce::MouseEvent&);
    bool finishRowDrag (RowRef);          // true: the click is used up (a drag, Ctrl/Shift, a resolved deferred click)
    void computeDropTarget (int yInContainer);
    std::vector<AudioEngine::TreeNode> draggedNodes (RowRef pressed) const;   // in visual order

    AudioEngine& engine;
    sidebar::VerticalScroll& vscroll;
    int lastScrollRevision = -1;
    juce::Viewport viewport;

    // Hears the wheel over the rows (the viewport scrolls them; Ctrl+Shift zooms the track height)
    struct WheelZoom final : juce::MouseListener
    {
        explicit WheelZoom (TrackList& o) : owner (o) {}
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        TrackList& owner;
    } wheelZoom;
    RowContainer rowContainer { *this };

    std::vector<AudioEngine::SidebarItem> items;          // what the rows are built from
    std::vector<int> rowTops;                             // parallel to items (container y)
    int totalHeight = 0;

    // Virtualized: components exist only for rows near the visible area
    using RowKey = std::tuple<int, int, int, int, int>;   // (folder, member, depth, instrument, channel)
    std::map<RowKey, std::unique_ptr<juce::Component>> liveRows;
    void realizeVisibleRows();

    AudioEngine::TrackId selectedTrack = 0;

    // Multi-select (UI-level; the primary selection stays with MainComponent)
    std::set<AudioEngine::TrackId> multiSelection;
    std::set<AudioEngine::FolderId> selectedFolders;
    std::set<AudioEngine::InstrumentId> selectedInstruments;
    std::set<AudioEngine::AudioChannelId> selectedChannels;   // audio rows
    RowRef anchor;                                            // where a Shift range starts (0: none)
    bool clearSelectionOnMouseUp = false, pressUsed = false;

    struct DragState
    {
        bool active = false;              // true once the mouse left the pressed row
        std::vector<AudioEngine::TreeNode> nodes;
        AudioEngine::InstrumentId reorderIn = 0;   // only tracks of this instrument: reordered in it
        std::vector<AudioEngine::TrackId> reordered;

        bool valid = false, intoFolder = false;
        AudioEngine::FolderId parent = 0;
        int index = 0;
        int indicatorY = -1;              // container coords
        juce::Rectangle<int> folderHighlight;
    } drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackList)
};
