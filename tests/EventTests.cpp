#include "../src/api/CommandDispatcher.h"
#include "../src/api/ApiServer.h"
#include "../src/api/EventBroadcaster.h"

// Subscribes over a real socket and checks that engine mutations arrive as events.
class EventTests final : public juce::UnitTest
{
public:
    EventTests() : UnitTest ("Event stream") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher dispatcher (engine);
        ApiServer server (dispatcher);
        EventBroadcaster broadcaster (engine, server);

        // The application wires this fan-out in Main.cpp; tests do it themselves.
        engine.eventSink = [&server] (const juce::var& event) { server.broadcastEvent (event); };

        expect (server.start (53913), "couldn't listen on the test port");

        juce::StreamingSocket client;
        expect (client.connect ("127.0.0.1", 53913, 3000), "couldn't connect");

        auto send = [&client] (const juce::String& line)
        {
            const auto data = line + "\n";
            client.write (data.toRawUTF8(), (int) data.getNumBytesAsUTF8());
        };

        juce::String received;

        auto pumpAndCollect = [&] (int milliseconds)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

            while (juce::Time::getMillisecondCounter() < deadline)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

                if (client.waitUntilReady (true, 0) == 1)
                {
                    char buffer[8192];
                    const auto numRead = client.read (buffer, sizeof (buffer), false);

                    if (numRead > 0)
                        received += juce::String::fromUTF8 (buffer, numRead);
                }
            }
        };

        auto countEvents = [&received] (const juce::String& type)
        {
            int count = 0;

            for (auto& line : juce::StringArray::fromLines (received))
                if (juce::JSON::parse (line).getProperty ("event", {}).toString() == type)
                    ++count;

            return count;
        };

        beginTest ("subscribe is acknowledged and events flow");
        {
            send (R"({"id":1,"cmd":"subscribe"})");
            pumpAndCollect (300);
            expect (received.contains ("\"ok\""), "no subscribe ack");

            dispatcher.run ("track.create", [] { auto o = new juce::DynamicObject();
                                                 o->setProperty ("name", "Evt"); return juce::var (o); }());
            dispatcher.run ("tempo.set", [] { auto o = new juce::DynamicObject();
                                              o->setProperty ("bpm", 101.0); return juce::var (o); }());
            pumpAndCollect (400);

            expect (countEvents ("trackAdded") >= 1, "no trackAdded event");
            expect (countEvents ("tempoChanged") >= 1, "no tempoChanged event");
            expect (countEvents ("trackChanged") >= 1, "no trackChanged (armed) event");
        }

        beginTest ("clip edits and markers emit");
        {
            received.clear();
            const auto trackId = engine.getTrackIds().front();
            engine.addToTrackSequence (trackId, { { 0, 960000, 1, 60, 100 } }, {});
            engine.addMarker (0, "intro");
            engine.removeMarker (0);
            pumpAndCollect (300);

            expect (countEvents ("clipChanged") >= 1, "no clipChanged event");
            expect (countEvents ("markerAdded") >= 1, "no markerAdded event");
            expect (countEvents ("markerRemoved") >= 1, "no markerRemoved event");
        }

        beginTest ("transport state changes arrive (device permitting)");
        {
            if (engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
            {
                logMessage ("!!! no audio device - skipping transport events");
            }
            else
            {
                received.clear();
                engine.getTransport().play();
                pumpAndCollect (700);
                engine.getTransport().stop();
                pumpAndCollect (400);

                expect (countEvents ("transport") >= 2, "expected play + stop transport events");

                juce::var playEvent;
                for (auto& line : juce::StringArray::fromLines (received))
                {
                    const auto parsed = juce::JSON::parse (line);
                    if (parsed.getProperty ("event", {}).toString() == "transport"
                         && (bool) parsed.getProperty ("playing", false))
                        playEvent = parsed;
                }

                expect (! playEvent.isVoid(), "no playing=true transport event");
                expect (playEvent.hasProperty ("positionTicks") && playEvent.hasProperty ("bar"));
            }
        }

        beginTest ("unsubscribe stops the flow");
        {
            send (R"({"id":2,"cmd":"unsubscribe"})");
            pumpAndCollect (200);
            received.clear();

            engine.addMarker (960000, "nope");
            pumpAndCollect (300);
            expect (countEvents ("markerAdded") == 0, "event arrived after unsubscribe");
        }

        engine.eventSink = nullptr;
        server.shutdown();
    }
};

static EventTests eventTests;
