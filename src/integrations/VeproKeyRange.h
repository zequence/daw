#pragma once

#include "VeproServer.h"
#include <zstd.h>

// A Synchron Player's playable key range, read from the server. Not a VST
// parameter: channel/instrument/state/export returns the player's state, whose
// core is zstd-compressed JSON. Its sampler tree carries rangeFrom/rangeTo per
// sound slot (the leaves); we read the first loaded slot's. Other players (Pianos,
// third-party plugins) don't expose this.
namespace vepro
{
    inline bool isSynchronPlayer (const juce::String& pluginId)
    {
        return pluginId == "Vienna Synchron Player";
    }

    // Decompresses the first zstd frame found in 'state' (empty on failure)
    inline juce::String decompressEmbeddedZstd (const juce::MemoryBlock& state)
    {
        static constexpr unsigned char magic[] = { 0x28, 0xb5, 0x2f, 0xfd };
        const auto* bytes = static_cast<const unsigned char*> (state.getData());
        const auto size = state.getSize();

        size_t start = 0;

        while (start + 4 <= size && std::memcmp (bytes + start, magic, 4) != 0)
            ++start;

        if (start + 4 > size)
            return {};

        auto* stream = ZSTD_createDStream();

        if (stream == nullptr)
            return {};

        juce::MemoryOutputStream out;
        std::vector<char> buffer (ZSTD_DStreamOutSize());
        ZSTD_inBuffer in { bytes + start, size - start, 0 };
        constexpr size_t maxOutput = 128 * 1024 * 1024;
        bool ok = false;

        for (;;)
        {
            ZSTD_outBuffer o { buffer.data(), buffer.size(), 0 };
            const auto result = ZSTD_decompressStream (stream, &o, &in);

            if (ZSTD_isError (result))
                break;

            out.write (buffer.data(), o.pos);

            if (result == 0)    // frame complete
            {
                ok = true;
                break;
            }

            if ((in.pos >= in.size && o.pos < o.size) || out.getDataSize() > maxOutput)
                break;          // truncated input / runaway output
        }

        ZSTD_freeDStream (stream);
        return ok ? out.toUTF8() : juce::String();
    }

    // The playable range of the FIRST LOADED sound slot: the first leaf of the
    // sampler tree (depth first) that has a patch loaded. Leaves are the sound
    // slots; each has its own rangeFrom/rangeTo. Empty slots (the "Custom" ones a
    // fresh player carries) have no patchEntry and report 0-127, so they are
    // skipped - they would otherwise stretch the range over the whole keyboard.
    // Later, with articulations, the range of the ACTIVE slot is what we want.
    // false when there is no loaded slot.
    inline bool keyRangeFromStateJson (const juce::var& document, int& low, int& high)
    {
        const auto data = document.getProperty ("data", {}).isObject() ? document.getProperty ("data", {}) : document;
        const auto root = data.getProperty ("custom", {}).getProperty ("sampler", {}).getProperty ("rootNode", {});

        if (! root.isObject())
            return false;

        std::function<bool (const juce::var&)> findFirstLoadedSlot = [&] (const juce::var& node)
        {
            const auto* children = node.getProperty ("nodes", {}).getArray();

            if (children != nullptr && ! children->isEmpty())
            {
                for (auto& child : *children)
                    if (findFirstLoadedSlot (child))
                        return true;

                return false;
            }

            const auto emptySlot = node.hasProperty ("patchEntry") && node.getProperty ("patchEntry", {}).toString().isEmpty();

            if (emptySlot || ! node.hasProperty ("rangeFrom") || ! node.hasProperty ("rangeTo"))
                return false;

            low = juce::jlimit (0, 127, (int) node.getProperty ("rangeFrom", 0));
            high = juce::jlimit (0, 127, (int) node.getProperty ("rangeTo", 127));
            return low <= high;
        };

        return findFirstLoadedSlot (root);
    }

    inline bool keyRangeFromState (const juce::MemoryBlock& state, int& low, int& high)
    {
        const auto json = decompressEmbeddedZstd (state);
        return json.isNotEmpty() && keyRangeFromStateJson (juce::JSON::parse (json), low, high);
    }

    // Blocking: export the player's state (two-plus CLI calls) and read its range.
    // false + 'error' set on failure.
    inline bool fetchKeyRange (const juce::File& cli, const juce::String& host, int port,
                               const juce::String& instanceId, const juce::String& channelAddress,
                               int& low, int& high, juce::String& error)
    {
        auto exportPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
        exportPayload->setProperty ("cmd", "channel/instrument/state/export");
        exportPayload->setProperty ("instanceId", instanceId);
        exportPayload->setProperty ("channelAddress", channelAddress);

        const auto exported = serverCall (cli, host, port, juce::var (exportPayload.get()), error);

        if (error.isNotEmpty())
            return false;

        const auto blobId = exported.getProperty ("blobRef", {}).getProperty ("blobId", {}).toString();
        const auto total = (juce::int64) exported.getProperty ("bytes", 0);

        if (blobId.isEmpty() || total <= 0)
        {
            error = "the server returned no instrument state";
            return false;
        }

        juce::MemoryBlock state;

        while ((juce::int64) state.getSize() < total)
        {
            auto blobRef = juce::DynamicObject::Ptr (new juce::DynamicObject());
            blobRef->setProperty ("blobId", blobId);

            auto readPayload = juce::DynamicObject::Ptr (new juce::DynamicObject());
            readPayload->setProperty ("cmd", "api/blob/read");
            readPayload->setProperty ("blobRef", juce::var (blobRef.get()));
            readPayload->setProperty ("offset", (juce::int64) state.getSize());
            readPayload->setProperty ("length", 1 << 20);

            const auto chunk = serverCall (cli, host, port, juce::var (readPayload.get()), error);

            if (error.isNotEmpty())
                return false;

            juce::MemoryOutputStream decoded;

            if (! juce::Base64::convertFromBase64 (decoded, chunk.getProperty ("base64", {}).toString())
                 || decoded.getDataSize() == 0)
                break;

            state.append (decoded.getData(), decoded.getDataSize());
        }

        if (! keyRangeFromState (state, low, high))
        {
            error = "no key range in the player's state";
            return false;
        }

        return true;
    }
}
