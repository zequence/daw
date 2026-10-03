#include "../src/AudioEngine.h"
#include "TestFlags.h"

// Replace-on-first-input recording, end to end with a real device: existing material
// before the first played note survives, material under the take is erased, material
// after the stop point survives, and undo restores the pre-take clip.
//
// "Performer" input is injected through the live MIDI collector, exactly like the
// on-screen keyboard. Runs in real time (~4 s); skips without a device.
class ReplaceModeTests final : public juce::UnitTest
{
public:
    ReplaceModeTests() : UnitTest ("Replace recording") {}

    void runTest() override
    {
        constexpr auto Q = Ticks::perQuarterNote;

        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        enginePtr = &engine;

        beginTest ("replace erases from first input to stop, no further");

        if (skipAudibleTests)
        {
            logMessage ("!!! --quiet: skipping audible test");
            return;
        }

        if (engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
        {
            logMessage ("!!! No audio device - skipping");
            return;
        }

        engine.setTempoBpm (240.0);   // keep the realtime part short: 1 bar = 1 s

        const auto track = engine.addTrack();
        engine.setTrackSequence (track, MidiSequence::create (
            { { 0, Q, 1, 60, 100 },        // bar 1: before the take - must survive
              { 8 * Q, Q, 1, 64, 100 },    // bar 3: under the take - must be erased
              { 16 * Q, Q, 1, 67, 100 } }, // bar 5: after the stop - must survive
            {}));

        engine.setTrackRecordReplace (track, true);
        engine.getTransport().locate (4 * Q);        // start at bar 2
        expect (engine.startRecording(), "recording didn't start");

        // Let it roll into bar 2 before "playing" anything
        pumpUntil ([&] { return engine.getTransport().getPositionTicks() >= 5 * Q; }, 4000);

        auto stamp = [] (juce::MidiMessage m) { m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001); return m; };
        engine.getLiveMidiCollector().addMessageToQueue (stamp (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 90)));

        pumpUntil ([&] { return engine.getTransport().getPositionTicks() >= 6 * Q; }, 3000);
        engine.getLiveMidiCollector().addMessageToQueue (stamp (juce::MidiMessage::noteOff (1, 72)));

        // Stop inside bar 4, before the bar-5 note
        pumpUntil ([&] { return engine.getTransport().getPositionTicks() >= 13 * Q; }, 6000);
        engine.stopRecording();
        engine.getTransport().stop();
        pump (300);

        const auto sequence = engine.getTrackSequence (track);
        expect (sequence != nullptr, "no clip after recording");

        if (sequence == nullptr)
            return;

        auto hasKey = [&sequence] (int key)
        {
            for (auto& note : sequence->getNotes())
                if (note.key == key)
                    return true;
            return false;
        };

        expect (hasKey (60), "note before the take was lost");
        expect (! hasKey (64), "note under the take was not erased");
        expect (hasKey (67), "note after the stop point was lost");
        expect (hasKey (72), "the played take is missing");

        // One undo restores the pre-take clip
        expect (engine.undoTrackSequence (track), "nothing to undo");
        const auto restored = engine.getTrackSequence (track);
        expect (restored != nullptr && restored->getNotes().size() == 3, "undo didn't restore the pre-take clip");
    }

private:
    static void pump (int milliseconds)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    }

    template <typename Condition>
    void pumpUntil (Condition&& done, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! done() && juce::Time::getMillisecondCounter() < deadline)
        {
            enginePtr->pollRecording();   // the shell's timer does this in the app
            pump (30);
        }
    }

public:
    AudioEngine* enginePtr = nullptr;
};

static ReplaceModeTests replaceModeTests;
