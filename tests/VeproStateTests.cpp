#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/integrations/VeproState.h"

// The synthesized VE Pro connection state must survive its own wrapping: VC2!
// container -> VST3PluginState XML -> juce-base64 -> 'Size' + JSON with the
// connection fields. (The live connect itself was verified against Pro Server
// 8.1 on 2026-10-03; this keeps the builder honest without a server.)
class VeproStateTests final : public juce::UnitTest
{
public:
    VeproStateTests() : UnitTest ("VE Pro state synthesis") {}

    void runTest() override
    {
        beginTest ("8.1 state round-trips through its wrapping");
        {
            vepro::ConnectTarget target;
            target.instanceName = "Horns";
            target.hostAddress = "192.168.1.9";
            target.hostName = "rig";
            target.decoupled = true;

            const auto blob = vepro::buildConnectionState ("8.1", target);
            expect (blob.getSize() > 0);

            // VC2! container (juce copyXmlToBinary layout)
            expect (std::memcmp (blob.getData(), "VC2!", 4) == 0);

            const auto text = juce::String::fromUTF8 (static_cast<const char*> (blob.getData()) + 8);
            const auto xml = juce::parseXML (text);
            expect (xml != nullptr && xml->hasTagName ("VST3PluginState"));

            juce::MemoryBlock component;
            expect (component.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText().trim()));
            expect (component.getSize() > 8);
            expect (std::memcmp (component.getData(), "Size", 4) == 0);

            const auto json = juce::String::fromUTF8 (static_cast<const char*> (component.getData()) + 8,
                                                      (int) component.getSize() - 8);
            const auto parsed = juce::JSON::parse (json);
            const auto custom = parsed["data"]["custom"];

            expectEquals (custom["instanceName"].toString(), juce::String ("Horns"));
            expectEquals (custom["hostAddress"].toString(), juce::String ("192.168.1.9"));
            expectEquals (custom["hostName"].toString(), juce::String ("rig"));
            expect ((bool) custom["decoupled"]);
            expect (custom["id"].toString().length() == 32);
        }

        beginTest ("unsupported versions return empty (callers list the supported set)");
        {
            expect (vepro::buildConnectionState ("7.0", { "X" }).getSize() == 0);
            expect (vepro::supportedVersions().contains (vepro::defaultVersion()));
        }
    }
};

static VeproStateTests veproStateTests;
