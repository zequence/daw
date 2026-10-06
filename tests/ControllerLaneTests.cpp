#include "../src/ui/ControllerLanes.h"

// Controller lanes: names, availability and their storage; aftertouch in the clip
class ControllerLaneTests final : public juce::UnitTest
{
public:
    ControllerLaneTests() : UnitTest ("Controller lanes") {}

    void runTest() override
    {
        beginTest ("every lane is listed; ids parse; standard names; CC names are separate from their numbers");
        {
            expectEquals (lanes::allIds().size(), 3 + 128);
            expect (lanes::parse ("cc11").kind == lanes::Kind::controller && lanes::parse ("cc11").cc == 11);
            expect (lanes::parse ("aftertouch").kind == lanes::Kind::aftertouch);
            expectEquals (lanes::parse ("pitchBend").maxValue(), 16383);
            expectEquals (lanes::defaultName ("cc7"), juce::String ("Volume"));
            expectEquals (lanes::defaultName ("cc21"), juce::String());   // undefined: no name yet
        }

        beginTest ("availability and names persist, and go back to the standard ones");
        {
            juce::PropertySet settings;
            auto& lanesSettings = lanes::Settings::get();
            lanesSettings.load (settings);

            expect (lanesSettings.isAvailable ("cc1") && ! lanesSettings.isAvailable ("cc21"));
            expectEquals (lanesSettings.displayName ("cc21"), juce::String ("CC21"));

            lanesSettings.setAvailable ("cc21", true);
            lanesSettings.setName ("cc21", "Vibrato");
            lanesSettings.setName ("cc1", "Dynamics");

            lanesSettings.load (settings);   // read back
            expect (lanesSettings.isAvailable ("cc21"));
            expectEquals (lanesSettings.displayName ("cc21"), juce::String ("CC21 Vibrato"));
            expectEquals (lanesSettings.name ("cc1"), juce::String ("Dynamics"));
            expect (lanesSettings.availableIds().contains ("cc21"));

            lanesSettings.setName ("cc1", "");   // empty: the standard name again
            expectEquals (lanesSettings.name ("cc1"), juce::String ("Modulation"));

            static juce::PropertySet empty;
            lanesSettings.load (empty);   // defaults for the other tests
        }

        beginTest ("aftertouch is kept in the clip (project files) and shown by its lane");
        {
            const auto seq = MidiSequence::create ({}, { { 0, MidiSequence::ControlType::aftertouch, 1, 0, 90 } });
            const auto loaded = MidiSequence::fromXml (*seq->toXml());
            expect (loaded->getControls().front().type == MidiSequence::ControlType::aftertouch);
            expectEquals (loaded->getControls().front().value, 90);
            expect (lanes::parse ("aftertouch").shows (loaded->getControls().front()));
            expect (! lanes::parse ("cc1").shows (loaded->getControls().front()));
        }
    }
};

static ControllerLaneTests controllerLaneTests;
