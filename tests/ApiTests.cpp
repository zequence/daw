#include "../src/api/CommandDispatcher.h"
#include "../src/api/ApiServer.h"
#include "../src/model/DemoSequence.h"

namespace
{
    // Dispatch a command and return the (synchronously delivered) reply.
    juce::var send (CommandDispatcher& dispatcher, const juce::String& cmd, const juce::String& paramsJson = "{}")
    {
        juce::var reply;
        dispatcher.dispatch (R"({"id":7,"cmd":")" + cmd + R"(","params":)" + paramsJson + "}",
                             [&reply] (const juce::var& r) { reply = r; });
        return reply;
    }

    bool isOk (const juce::var& reply)
    {
        return reply.getProperty ("ok", false);
    }

    juce::var result (const juce::var& reply)
    {
        return reply.getProperty ("result", {});
    }
}

//==============================================================================
class ApiTests final : public juce::UnitTest
{
public:
    ApiTests() : UnitTest ("Command API") {}

    void runTest() override
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        juce::PropertiesFile settings (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                           .getChildFile ("OrchestralDAWTestSettings.xml"), options);

        AudioEngine engine (settings);
        CommandDispatcher dispatcher (engine);

        beginTest ("describe lists commands and echoes the request id");
        {
            const auto reply = send (dispatcher, "describe");
            expect (isOk (reply));
            expectEquals ((int) reply.getProperty ("id", 0), 7);
            expect (result (reply).getProperty ("commands", {}).getArray() != nullptr);
            expectEquals ((juce::int64) result (reply).getProperty ("ticksPerQuarterNote", 0), Ticks::perQuarterNote);
        }

        beginTest ("malformed input and unknown commands fail politely");
        {
            juce::var reply;
            dispatcher.dispatch ("this is not json", [&reply] (const juce::var& r) { reply = r; });
            expect (! isOk (reply));

            const auto unknown = send (dispatcher, "flux.capacitate");
            expect (! isOk (unknown));
            expect (unknown.getProperty ("error", {}).toString().contains ("track.create"));
        }

        beginTest ("track lifecycle through the API");
        {
            const auto created = send (dispatcher, "track.create", R"({"name":"Horns"})");
            expect (isOk (created));
            const auto trackId = (int) result (created).getProperty ("id", 0);
            expect (trackId > 0);

            expect (isOk (send (dispatcher, "track.rename",
                                R"({"trackId":)" + juce::String (trackId) + R"(,"name":"Horns a2"})")));
            expectEquals (engine.getTrackName (trackId), juce::String ("Horns a2"));

            expect (isOk (send (dispatcher, "track.setMuted",
                                R"({"trackId":)" + juce::String (trackId) + R"(,"muted":true})")));
            expect (engine.isTrackMuted (trackId));

            const auto list = result (send (dispatcher, "track.list"));
            expect (list.getArray() != nullptr && list.getArray()->size() == 1);
            expectEquals (list[0].getProperty ("name", {}).toString(), juce::String ("Horns a2"));
            expect (list[0].getProperty ("armed", false));

            const auto bad = send (dispatcher, "track.rename", R"({"trackId":999,"name":"x"})");
            expect (! isOk (bad));
            expect (bad.getProperty ("error", {}).toString().contains ("999"));
        }

        beginTest ("clip editing through the API");
        {
            const auto trackId = engine.getTrackIds().front();
            const auto id = juce::String (trackId);

            const auto added = send (dispatcher, "clip.addNotes",
                R"({"trackId":)" + id + R"(,"notes":[
                     {"start":0,"length":960000,"key":60,"velocity":90},
                     {"start":960000,"length":480000,"key":64}],
                    "controls":[{"tick":0,"type":0,"number":1,"value":80}]})");
            expect (isOk (added));
            expectEquals ((int) result (added).getProperty ("noteCount", 0), 2);

            const auto clip = result (send (dispatcher, "clip.get", R"({"trackId":)" + id + "}"));
            expect (clip.getProperty ("notes", {}).getArray() != nullptr);
            expectEquals ((int) clip["notes"].getArray()->size(), 2);
            expectEquals ((int) clip["notes"][0].getProperty ("key", 0), 60);
            expectEquals ((int) clip.getProperty ("controlCount", 0), 1);

            const auto rejected = send (dispatcher, "clip.addNotes",
                                        R"({"trackId":)" + id + R"(,"notes":[{"start":0}]})");
            expect (! isOk (rejected));

            expect (isOk (send (dispatcher, "clip.clear", R"({"trackId":)" + id + "}")));
            expect (engine.getTrackSequence (trackId) == nullptr);
        }

        beginTest ("transport and tempo through the API");
        {
            expect (isOk (send (dispatcher, "tempo.set", R"({"bpm":132.5})")));
            expectWithinAbsoluteError (engine.getTempoBpm(), 132.5, 0.01);

            expect (isOk (send (dispatcher, "transport.locate", R"({"bar":3})")));

            if (engine.getDeviceManager().getCurrentAudioDevice() != nullptr)
            {
                // The audio thread applies locates; give it a few blocks.
                const auto deadline = juce::Time::getMillisecondCounter() + 2000;
                juce::var status;

                while (juce::Time::getMillisecondCounter() < deadline)
                {
                    status = result (send (dispatcher, "transport.status"));

                    if ((int) status.getProperty ("bar", 0) == 3)
                        break;

                    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
                }

                expectEquals ((int) status.getProperty ("bar", 0), 3);
                expectWithinAbsoluteError ((double) status.getProperty ("bpm", 0.0), 132.5, 0.01);
            }

            expect (! isOk (send (dispatcher, "tempo.set", R"({"bpm":-4})")));
        }

        beginTest ("project.new runs the UI hooks");
        {
            int before = 0, after = 0;
            dispatcher.onBeforeProjectChange = [&before] { ++before; };
            dispatcher.onAfterProjectChange = [&after] (const juce::File&) { ++after; };

            expect (isOk (send (dispatcher, "project.new")));
            expectEquals (before, 1);
            expectEquals (after, 1);
            expect (engine.getTrackIds().empty());
        }

        beginTest ("end to end over a real socket");
        {
            ApiServer server (dispatcher);
            expect (server.start (53911), "couldn't listen on the test port");

            juce::StreamingSocket client;
            expect (client.connect ("127.0.0.1", 53911, 3000), "couldn't connect");

            const juce::String request = R"({"id":42,"cmd":"track.create","params":{"name":"Socket"}})" "\n";
            client.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8());

            // The server dispatches on the message thread, so pump while waiting.
            juce::String received;
            const auto deadline = juce::Time::getMillisecondCounter() + 5000;

            while (! received.containsChar ('\n') && juce::Time::getMillisecondCounter() < deadline)
            {
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

                if (client.waitUntilReady (true, 0) == 1)
                {
                    char buffer[1024];
                    const auto numRead = client.read (buffer, sizeof (buffer), false);

                    if (numRead > 0)
                        received += juce::String::fromUTF8 (buffer, numRead);
                }
            }

            const auto reply = juce::JSON::parse (received.upToFirstOccurrenceOf ("\n", false, false));
            expect (isOk (reply), "socket round trip failed: " + received);
            expectEquals ((int) reply.getProperty ("id", 0), 42);
            expectEquals (result (reply).getProperty ("name", {}).toString(), juce::String ("Socket"));
            expectEquals ((int) engine.getTrackIds().size(), 1);

            server.shutdown();
        }
    }
};

static ApiTests apiTests;
