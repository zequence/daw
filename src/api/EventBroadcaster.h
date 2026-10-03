#pragma once

#include "../AudioEngine.h"
#include "ApiServer.h"

// Watches the transport (whose state changes on the audio thread) from a
// message-thread timer and broadcasts to subscribed API connections:
//  - a "transport" event whenever play/record/loop state changes
//  - a "position" event every 500 ms while playing
// Engine mutation events reach the server through the application's event fan-out
// (see Main.cpp), not through this class.
class EventBroadcaster final : private juce::Timer
{
public:
    EventBroadcaster (AudioEngine& e, ApiServer& s) : engine (e), server (s)
    {
        startTimerHz (20);
    }

    ~EventBroadcaster() override
    {
        stopTimer();
    }

private:
    struct TransportState
    {
        bool playing = false, recording = false, looping = false;
        juce::int64 loopStart = 0, loopEnd = 0;

        bool operator== (const TransportState& other) const
        {
            return playing == other.playing && recording == other.recording && looping == other.looping
                && loopStart == other.loopStart && loopEnd == other.loopEnd;
        }
    };

    void timerCallback() override
    {
        auto& transport = engine.getTransport();
        const TransportState now { transport.isPlaying(), engine.isRecording(), transport.isLooping(),
                                   transport.getLoopStart(), transport.getLoopEnd() };

        if (! (now == last))
        {
            last = now;
            lastPositionMs = juce::Time::getMillisecondCounter();
            server.broadcastEvent (makeEvent ("transport", now));
        }
        else if (now.playing && juce::Time::getMillisecondCounter() - lastPositionMs >= 500)
        {
            lastPositionMs = juce::Time::getMillisecondCounter();
            server.broadcastEvent (makeEvent ("position", now));
        }
    }

    juce::var makeEvent (const juce::String& type, const TransportState& state) const
    {
        auto& transport = engine.getTransport();
        const auto ticks = transport.getPositionTicks();
        const auto position = transport.getTempoMap()->ticksToBarsBeats (ticks);

        auto data = new juce::DynamicObject();
        data->setProperty ("event", type);
        data->setProperty ("playing", state.playing);
        data->setProperty ("recording", state.recording);
        data->setProperty ("looping", state.looping);
        data->setProperty ("positionTicks", ticks);
        data->setProperty ("bar", position.bar);
        data->setProperty ("beat", position.beat);
        data->setProperty ("bpm", engine.getTempoBpm());
        return juce::var (data);
    }

    AudioEngine& engine;
    ApiServer& server;
    TransportState last;
    juce::uint32 lastPositionMs = 0;

    JUCE_DECLARE_NON_COPYABLE (EventBroadcaster)
};
