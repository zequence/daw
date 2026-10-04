#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/integrations/VeproState.h"
#include "../src/integrations/VeproKeyRange.h"
#include "../src/engine/MidiRouteProcessor.h"

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

        beginTest ("multiport MIDI: port >= 2 wraps for the patched VST3 host, port 1 stays plain");
        {
            const auto noteOn = juce::MidiMessage::noteOn (3, 60, (juce::uint8) 100);

            const auto plain = MidiRouteProcessor::wrapForPort (noteOn, 1);
            expect (plain.isNoteOn() && plain.getChannel() == 3);

            const auto wrapped = MidiRouteProcessor::wrapForPort (noteOn, 5);
            const auto* raw = wrapped.getRawData();
            expect (wrapped.isSysEx());
            expectEquals (wrapped.getRawDataSize(), 9);
            expect (raw[0] == 0xf0 && raw[1] == 0x7d && raw[2] == 0x50);
            expectEquals ((int) raw[3], 4);                                      // event bus = port - 1
            expectEquals ((int) ((raw[4] << 4) | raw[5]), (int) noteOn.getRawData()[0]);
            expect (raw[6] == 60 && raw[7] == 100 && raw[8] == 0xf7);

            // Every wrapped byte stays 7-bit (it lives inside a SysEx)
            for (int i = 1; i < wrapped.getRawDataSize() - 1; ++i)
                expect (raw[i] < 0x80);
        }

        beginTest ("unsupported versions return empty (callers list the supported set)");
        {
            expect (vepro::buildConnectionState ("7.0", { "X" }).getSize() == 0);
            expect (vepro::supportedVersions().contains (vepro::defaultVersion()));
        }

        beginTest ("Synchron key range: zstd JSON inside the state, first loaded sound slot");
        {
            // "HDR\0\1" + zstd(JSON: Stac 72-108 > x 74-109, Leg 73-100) + "TAIL"; the first leaf is x
            static constexpr unsigned char state[] = {
                0x48, 0x44, 0x52, 0x00, 0x01, 0x28, 0xb5, 0x2f, 0xfd, 0x20, 0xf0, 0xf5, 0x03, 0x00, 0xa2, 0xc5,
                0x14, 0x1b, 0x60, 0x8b, 0xda, 0x71, 0x15, 0x65, 0xa7, 0xdb, 0x73, 0x46, 0xb7, 0x63, 0xcb, 0xb6,
                0x41, 0x13, 0x8e, 0x89, 0x36, 0x1a, 0x27, 0x2b, 0x92, 0xa3, 0x28, 0xb8, 0x04, 0xff, 0xff, 0xe5,
                0x49, 0xa0, 0xc1, 0x03, 0xf2, 0xe5, 0x05, 0x81, 0x1f, 0x84, 0x44, 0xb1, 0xe4, 0x54, 0x00, 0x06,
                0x6a, 0xc0, 0x06, 0x3a, 0x65, 0x6e, 0x13, 0x0b, 0x4b, 0x5e, 0xa8, 0x12, 0xb2, 0x21, 0x76, 0x96,
                0xb1, 0xa8, 0x12, 0xb1, 0x99, 0xaa, 0xe8, 0x34, 0x9e, 0x5b, 0x4f, 0xee, 0x72, 0x30, 0x8e, 0x2c,
                0xf9, 0xf6, 0x94, 0xa3, 0x10, 0x00, 0xa0, 0x13, 0x58, 0xe7, 0xf1, 0x30, 0x9c, 0x82, 0xce, 0x5d,
                0x11, 0xcb, 0x94, 0xc0, 0x40, 0xa8, 0x9c, 0xe6, 0xe1, 0x0b, 0x8b, 0x01, 0x81, 0xf8, 0x69, 0x9f,
                0xf3, 0x42, 0x8b, 0x11, 0x98, 0x8d, 0xbf, 0x88, 0xe9, 0xab, 0x2c, 0x01, 0x54, 0x41, 0x49, 0x4c };

            int low = -1, high = -1;
            expect (vepro::keyRangeFromState (juce::MemoryBlock (state, sizeof (state)), low, high));
            expectEquals (low, 74);
            expectEquals (high, 109);

            // No zstd frame, or a truncated one: no range
            expect (! vepro::keyRangeFromState (juce::MemoryBlock (state, 5), low, high));
            expect (! vepro::keyRangeFromState (juce::MemoryBlock (state, 60), low, high));

            expect (vepro::isSynchronPlayer ("Vienna Synchron Player"));
            expect (! vepro::isSynchronPlayer ("Vienna Synchron Pianos"));
        }

        beginTest ("Synchron key range: first loaded slot, empty Custom slots are skipped (real state shape)");
        {
            const auto doc = juce::JSON::parse (R"({"data":{"custom":{"sampler":{"rootNode":{"subTitle":"Articulation","nodes":[
                {"subTitle":"Attack","nodes":[
                    {"patchEntry":"vol://x/01P stac short.vsynpatch","rangeFrom":55,"rangeTo":103},
                    {"patchEntry":"vol://x/02P stac agile.vsynpatch","rangeFrom":55,"rangeTo":103}]},
                {"subTitle":"Type","nodes":[
                    {"patchEntry":"vol://x/11P Long.vsynpatch","rangeFrom":58,"rangeTo":100}]},
                {"subTitle":"Custom","nodes":[
                    {"patchEntry":"","rangeFrom":0,"rangeTo":127},
                    {"patchEntry":"","rangeFrom":0,"rangeTo":127}]}]}}}}})");

            int low = -1, high = -1;
            expect (vepro::keyRangeFromStateJson (doc, low, high));
            expectEquals (low, 55);
            expectEquals (high, 103);

            // Empty slots come first: they are skipped, the first LOADED one wins
            const auto emptyFirst = juce::JSON::parse (R"({"custom":{"sampler":{"rootNode":{"nodes":[
                {"patchEntry":"","rangeFrom":0,"rangeTo":127},
                {"patchEntry":"vol://x/a.vsynpatch","rangeFrom":36,"rangeTo":96}]}}}})");
            expect (vepro::keyRangeFromStateJson (emptyFirst, low, high));
            expectEquals (low, 36);
            expectEquals (high, 96);

            // Nothing loaded: no range (the editor shows every key)
            const auto none = juce::JSON::parse (R"({"custom":{"sampler":{"rootNode":{"nodes":[
                {"patchEntry":"","rangeFrom":0,"rangeTo":127}]}}}})");
            expect (! vepro::keyRangeFromStateJson (none, low, high));
        }
    }
};

static VeproStateTests veproStateTests;
