#pragma once

#include <juce_core/juce_core.h>

// Vienna Ensemble Pro interop (MILESTONES.md "Vienna Ensemble Pro integration").
//
// A loaded VE Pro plugin connects to a server instance when it receives a state
// naming that instance. The state format is VSL-internal and may change between
// Pro Server releases, so everything here is keyed by an explicit VERSION string
// ("8.1" = Pro Server 8.1, the format this was verified against on 2026-10-03).
// New server releases get their own builder next to build81; the UI's
// Settings > Integrations selector and the instrument.connectVepro command pick
// the version. Knowledge is from observing states for interoperability only -
// no VSL code (see the licensing note in MILESTONES.md).
namespace vepro
{
    constexpr auto versionSettingsKey = "veproApiVersion";

    inline juce::StringArray supportedVersions()    { return { "8.1" }; }
    inline juce::String defaultVersion()            { return "8.1"; }

    struct ConnectTarget
    {
        juce::String instanceName;
        juce::String hostAddress { "127.0.0.1" };
        juce::String hostName { "localhost" };
        bool decoupled = true;              // connection only, no embedded instance content
        juce::String pluginId;              // 32 hex chars; empty = generate
    };

    namespace detail
    {
        // VSL's component state 'Size' + uint32 + JSON, inside JUCE's
        // <VST3PluginState> XML, inside the VC2! binary-XML container.
        inline juce::MemoryBlock wrap (const juce::var& root)
        {
            const auto json = juce::JSON::toString (root, true);

            juce::MemoryOutputStream component;
            component.write ("Size", 4);
            component.writeInt ((int) json.getNumBytesAsUTF8());
            component << json;

            const juce::MemoryBlock componentBlock (component.getData(), component.getDataSize());

            const auto xml = juce::String ("<?xml version=\"1.0\" encoding=\"UTF-8\"?> "
                                           "<VST3PluginState><IComponent>")
                               + componentBlock.toBase64Encoding()
                               + "</IComponent></VST3PluginState>";

            // juce::AudioProcessor::copyXmlToBinary layout: 'VC2!' + uint32 textLength + text + NUL
            juce::MemoryOutputStream out;
            out.writeInt (0x21324356);
            out.writeInt ((int) xml.getNumBytesAsUTF8());
            out << xml;
            out.writeByte (0);

            return out.getMemoryBlock();
        }

        // Pro Server 8.1: the VST3 component state is 'Size' + uint32 + JSON, carried
        // inside JUCE's <VST3PluginState> XML, inside the VC2! binary-XML container.
        inline juce::MemoryBlock build81 (const ConnectTarget& target)
        {
            auto* custom = new juce::DynamicObject();
            custom->setProperty ("arch64", true);
            custom->setProperty ("audioInputLag", 0);
            custom->setProperty ("decoupled", target.decoupled);
            custom->setProperty ("hostAddress", target.hostAddress);
            custom->setProperty ("hostName", target.hostName);
            custom->setProperty ("id", target.pluginId.isNotEmpty() ? target.pluginId
                                                                      : juce::Uuid().toString());   // 32 hex chars
            custom->setProperty ("instanceName", target.instanceName);
            custom->setProperty ("latencyBufferCount", 2);

            auto* gui = new juce::DynamicObject();
            gui->setProperty ("currentView", "main");
            gui->setProperty ("flow", juce::var());
            gui->setProperty ("height", 280);
            gui->setProperty ("scale", 1);
            gui->setProperty ("width", 420);

            auto* meta = new juce::DynamicObject();
            meta->setProperty ("gui", juce::var (gui));

            auto* data = new juce::DynamicObject();
            data->setProperty ("currentPreset", "");
            data->setProperty ("currentPresetSystem", false);
            data->setProperty ("custom", juce::var (custom));
            data->setProperty ("meta", juce::var (meta));
            data->setProperty ("parameters", juce::var (new juce::DynamicObject()));

            auto* root = new juce::DynamicObject();
            root->setProperty ("data", juce::var (data));
            root->setProperty ("version", 0);

            return wrap (juce::var (root));
        }
    }

    // Empty block = unsupported version (callers report supportedVersions()).
    inline juce::MemoryBlock buildConnectionState (const juce::String& version, const ConnectTarget& target)
    {
        if (version == "8.1")
            return detail::build81 (target);

        return {};
    }
}
