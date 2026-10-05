#include "../src/model/NoteNames.h"

class NoteNamesTests final : public juce::UnitTest
{
public:
    NoteNamesTests() : juce::UnitTest ("Note names", "Model") {}

    void runTest() override
    {
        beginTest ("MIDI 60 is C3, as VSL and Cubase name it");
        expectEquals (noteNames::name (60), juce::String ("C3"));
        expectEquals (noteNames::name (0), juce::String ("C-2"));
        expectEquals (noteNames::name (61), juce::String ("C#3"));
        expectEquals (noteNames::name (55), juce::String ("G2"));    // the violins' lowest note
        expectEquals (noteNames::name (127), juce::String ("G8"));

        beginTest ("the setting: middle C as C4 or C5");
        noteNames::middleCOctave() = 4;
        expectEquals (noteNames::name (60), juce::String ("C4"));
        expectEquals (noteNames::name (0), juce::String ("C-1"));
        noteNames::middleCOctave() = 5;
        expectEquals (noteNames::name (60), juce::String ("C5"));
        noteNames::middleCOctave() = 3;
    }
};

static NoteNamesTests noteNamesTests;
