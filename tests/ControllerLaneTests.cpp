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

        beginTest ("velocity colours get evenly lighter from soft to loud (no hue outshines the next)");
        {
            auto previous = -1.0f;
            bool rising = true;

            for (int v = 0; v < 128; v += 4)
            {
                const auto luminance = lanes::perceivedLuminance (lanes::valueColour (lanes::Kind::velocity, (float) v / 127.0f));

                if (luminance < previous - 0.005f)
                    rising = false;

                previous = luminance;
            }

            expect (rising, "a softer velocity looks brighter than a louder one");
            expect (lanes::perceivedLuminance (lanes::valueColour (lanes::Kind::velocity, 0.0f)) < 0.12f);   // dark
            expect (lanes::perceivedLuminance (lanes::valueColour (lanes::Kind::velocity, 1.0f)) > 0.55f);   // light
        }

        beginTest ("CC points: steps hold, ramps render for playback, bends shape them, files keep them");
        {
            constexpr auto Q = Ticks::perQuarterNote;
            using C = MidiSequence::Control;
            auto point = [] (juce::int64 tick, int value, bool ramp = false, float bend = 0.5f)
            {
                C c { tick, MidiSequence::ControlType::controller, 1, 11, value };
                c.ramp = ramp;
                c.bend = bend;
                return c;
            };

            // A step: nothing is added between the points
            const auto steps = MidiSequence::create ({}, { point (0, 0), point (4 * Q, 127) });
            expect (MidiSequence::withRampsRendered (steps) == steps);

            // A straight ramp: messages along it, rising, half way at the middle
            const auto ramp = MidiSequence::create ({}, { point (0, 0, true), point (4 * Q, 127) });
            const auto rendered = MidiSequence::withRampsRendered (ramp);
            expect (rendered->getControls().size() > 100);
            int previous = -1;
            bool rising = true;

            for (auto& c : rendered->getControls())
            {
                rising = rising && c.value >= previous;
                previous = c.value;
                expect (! c.ramp);
            }

            expect (rising);
            expectWithinAbsoluteError (MidiSequence::laneValueAt (ramp->getControls(), point (0, 0), 2 * Q), 64, 1);

            // Bent: the handle's height is the value at the middle
            const auto bent = MidiSequence::create ({}, { point (0, 0, true, 0.2f), point (4 * Q, 100) });
            expectWithinAbsoluteError (MidiSequence::laneValueAt (bent->getControls(), point (0, 0), 2 * Q), 20, 1);

            // Another CC's points are another lane
            const auto two = MidiSequence::create ({}, { point (0, 0, true), C { Q, MidiSequence::ControlType::controller, 1, 1, 90 },
                                                         point (4 * Q, 127) });
            expectWithinAbsoluteError (MidiSequence::laneValueAt (two->getControls(), point (0, 0), 2 * Q), 64, 1);

            // Project files keep the ramp and its bend; old ones (none) load as steps
            const auto loaded = MidiSequence::fromXml (*bent->toXml());
            expect (loaded->getControls()[0].ramp && std::abs (loaded->getControls()[0].bend - 0.2f) < 0.001f);
            expect (! loaded->getControls()[1].ramp);
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
