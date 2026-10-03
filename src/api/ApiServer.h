#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "CommandDispatcher.h"

// Localhost TCP server for the command API: newline-delimited JSON, one request object
// per line, one reply per request (see API.md). Commands execute on the message thread.
// Local connections only - the listener binds to 127.0.0.1.
class ApiServer final : private juce::Thread
{
public:
    explicit ApiServer (CommandDispatcher& d) : Thread ("API server"), dispatcher (d) {}

    ~ApiServer() override
    {
        shutdown();
    }

    bool start (int portToUse)
    {
        shutdown();

        if (! listener.createListener (portToUse, "127.0.0.1"))
            return false;

        port = portToUse;
        startThread();
        return true;
    }

    void shutdown()
    {
        signalThreadShouldExit();
        listener.close();
        stopThread (5000);

        const juce::ScopedLock lock (connectionsLock);

        for (auto& connection : connections)
            connection->close();

        connections.clear();
    }

    int getPort() const noexcept { return port; }

    // Push an event line to every connection that sent "subscribe". Message thread.
    void broadcastEvent (const juce::var& event)
    {
        const juce::ScopedLock lock (connectionsLock);

        bool anySubscribed = false;
        for (auto& connection : connections)
            anySubscribed = anySubscribed || connection->subscribed.load();

        if (! anySubscribed)
            return;

        const auto line = juce::JSON::toString (event, true);

        for (auto& connection : connections)
            if (connection->subscribed.load() && connection->isThreadRunning())
                connection->sendLine (line);
    }

private:
    //==============================================================================
    struct Connection final : juce::Thread,
                              std::enable_shared_from_this<Connection>
    {
        Connection (ApiServer& ownerToUse, juce::StreamingSocket* socketToUse)
            : Thread ("API connection"), owner (ownerToUse), socket (socketToUse)
        {
        }

        ~Connection() override { close(); }

        void close()
        {
            signalThreadShouldExit();
            socket->close();
            stopThread (3000);
        }

        void run() override
        {
            juce::MemoryBlock pending;

            while (! threadShouldExit())
            {
                const auto ready = socket->waitUntilReady (true, 100);

                if (ready < 0)
                    break;

                if (ready == 0)
                    continue;

                char buffer[4096];
                const auto numRead = socket->read (buffer, sizeof (buffer), false);

                if (numRead <= 0)
                    break;

                pending.append (buffer, (size_t) numRead);

                // Split complete lines out of the pending buffer
                for (;;)
                {
                    auto* data = static_cast<const char*> (pending.getData());
                    auto* newline = static_cast<const char*> (std::memchr (data, '\n', pending.getSize()));

                    if (newline == nullptr)
                        break;

                    const auto lineLength = (size_t) (newline - data);
                    const auto line = juce::String::fromUTF8 (data, (int) lineLength).trim();
                    pending.removeSection (0, lineLength + 1);

                    if (line.isNotEmpty())
                        handleLine (line);
                }

                if (pending.getSize() > maxLineBytes)
                    break;   // garbage flood; drop the connection
            }
        }

        void handleLine (const juce::String& line)
        {
            juce::MessageManager::callAsync (
                [dispatcher = juce::WeakReference<CommandDispatcher> (&owner.dispatcher),
                 self = shared_from_this(), line]
                {
                    if (dispatcher == nullptr)
                        return;

                    const auto parsed = juce::JSON::parse (line);
                    const auto cmd = parsed.getProperty ("cmd", {}).toString();

                    // Subscriptions are per-connection, so the server handles them itself.
                    if (cmd == "subscribe" || cmd == "unsubscribe")
                    {
                        self->subscribed.store (cmd == "subscribe");

                        auto reply = new juce::DynamicObject();
                        reply->setProperty ("ok", true);

                        if (const auto id = parsed.getProperty ("id", {}); ! id.isVoid())
                            reply->setProperty ("id", id);

                        self->sendLine (juce::JSON::toString (juce::var (reply), true));
                        return;
                    }

                    dispatcher->dispatchParsed (parsed, [self] (const juce::var& reply)
                    {
                        self->sendLine (juce::JSON::toString (reply, true));
                    });
                });
        }

        void sendLine (const juce::String& text)
        {
            const juce::ScopedLock lock (writeLock);
            const auto line = text + "\n";
            socket->write (line.toRawUTF8(), (int) line.getNumBytesAsUTF8());
        }

        static constexpr size_t maxLineBytes = 8 * 1024 * 1024;

        ApiServer& owner;
        std::unique_ptr<juce::StreamingSocket> socket;
        juce::CriticalSection writeLock;
        std::atomic<bool> subscribed { false };
    };

    //==============================================================================
    void run() override
    {
        while (! threadShouldExit())
        {
            auto* incoming = listener.waitForNextConnection();   // unblocked by listener.close()

            if (incoming == nullptr)
                continue;

            auto connection = std::make_shared<Connection> (*this, incoming);
            connection->startThread();

            const juce::ScopedLock lock (connectionsLock);
            std::erase_if (connections, [] (const auto& c) { return ! c->isThreadRunning(); });
            connections.push_back (std::move (connection));
        }
    }

    CommandDispatcher& dispatcher;
    juce::StreamingSocket listener;
    std::vector<std::shared_ptr<Connection>> connections;
    juce::CriticalSection connectionsLock;
    int port = 0;

    JUCE_DECLARE_NON_COPYABLE (ApiServer)
};
