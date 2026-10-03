#pragma once

#include "MidiRecorderProcessor.h"
#include "../model/MidiSequence.h"

// Message-thread side of recording: drains the recorder node's FIFO, pairs note-ons
// with note-offs, and collects controller data. poll() regularly while recording;
// finish() closes anything still held and returns everything gathered.
//
// Loop recording: the wrap marker closes held notes at the loop end, and the wrap flag
// lets the owner commit the pass so far, which makes each loop pass audible on the next.
class MidiRecorder
{
public:
    explicit MidiRecorder (MidiRecorderProcessor& p) : processor (p) {}

    struct Result
    {
        std::vector<MidiSequence::Note> notes;
        std::vector<MidiSequence::Control> controls;

        bool isEmpty() const noexcept { return notes.empty() && controls.empty(); }
    };

    void start (int track)
    {
        trackId = track;
        openNotes.clear();
        pending = {};
        wrapped = false;
        recording = true;
        processor.setActive (true);
    }

    bool isRecording() const noexcept   { return recording; }
    int getTrackId() const noexcept     { return trackId; }

    void poll()
    {
        processor.drain ([this] (const MidiRecorderProcessor::TimedEvent& event) { handle (event); });
    }

    // True once per loop wrap; combine with takePending() to commit the pass so far.
    bool consumeWrapFlag()
    {
        return std::exchange (wrapped, false);
    }

    Result takePending()
    {
        return std::exchange (pending, {});
    }

    // Stops capturing, closes held notes at 'endTick' and returns the remainder.
    Result finish (juce::int64 endTick)
    {
        processor.setActive (false);
        poll();
        closeAllOpenNotes (endTick);
        recording = false;
        return std::exchange (pending, {});
    }

private:
    struct OpenNote { int channel, key, velocity; juce::int64 startTick; };

    void handle (const MidiRecorderProcessor::TimedEvent& event)
    {
        if (event.wrapMarker)
        {
            closeAllOpenNotes (event.tick);
            wrapped = true;
            return;
        }

        const juce::MidiMessage message (event.data, event.size, 0.0);

        if (message.isNoteOn())   // note-on with velocity 0 counts as isNoteOff()
        {
            openNotes.push_back ({ message.getChannel(), message.getNoteNumber(),
                                   (int) message.getVelocity(), event.tick });
        }
        else if (message.isNoteOff())
        {
            const auto it = std::find_if (openNotes.begin(), openNotes.end(),
                                          [&message] (const OpenNote& note)
                                          {
                                              return note.channel == message.getChannel()
                                                  && note.key == message.getNoteNumber();
                                          });

            if (it != openNotes.end())   // unmatched offs (e.g. held across a loop wrap) are dropped
            {
                closeNote (*it, event.tick);
                openNotes.erase (it);
            }
        }
        else if (message.isController())
        {
            pending.controls.push_back ({ event.tick, MidiSequence::ControlType::controller,
                                          message.getChannel(), message.getControllerNumber(),
                                          message.getControllerValue() });
        }
        else if (message.isPitchWheel())
        {
            pending.controls.push_back ({ event.tick, MidiSequence::ControlType::pitchBend,
                                          message.getChannel(), 0, message.getPitchWheelValue() });
        }
        else if (message.isProgramChange())
        {
            pending.controls.push_back ({ event.tick, MidiSequence::ControlType::programChange,
                                          message.getChannel(), 0, message.getProgramChangeNumber() });
        }
    }

    void closeNote (const OpenNote& note, juce::int64 endTick)
    {
        pending.notes.push_back ({ note.startTick,
                                   juce::jmax ((juce::int64) 1, endTick - note.startTick),
                                   note.channel, note.key, note.velocity });
    }

    void closeAllOpenNotes (juce::int64 endTick)
    {
        for (const auto& note : openNotes)
            closeNote (note, endTick);

        openNotes.clear();
    }

    MidiRecorderProcessor& processor;
    std::vector<OpenNote> openNotes;
    Result pending;
    int trackId = 0;
    bool recording = false, wrapped = false;

    JUCE_DECLARE_NON_COPYABLE (MidiRecorder)
};
