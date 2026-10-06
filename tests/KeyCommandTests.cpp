#include "../src/ui/KeyCommands.h"

// The key command registry: KEY_COMMANDS.md documents every command, no two commands
// share a default key where they could collide, and the user's keys round-trip.
class KeyCommandTests final : public juce::UnitTest
{
public:
    KeyCommandTests() : UnitTest ("Key commands") {}

    void runTest() override
    {
        beginTest ("KEY_COMMANDS.md lists every command");
        {
            const auto doc = juce::File (__FILE__).getParentDirectory().getParentDirectory()
                                 .getChildFile ("KEY_COMMANDS.md").loadFileAsString();
            expect (doc.isNotEmpty(), "KEY_COMMANDS.md not found");

            for (auto& command : keys::commands())
                expect (doc.contains ("`" + juce::String (command.id) + "`"),
                        juce::String (command.id) + " is missing from KEY_COMMANDS.md");
        }

        beginTest ("ids are unique and the defaults don't collide");
        {
            std::set<juce::String> ids;

            for (auto& command : keys::commands())
            {
                expect (ids.insert (command.id).second, juce::String ("duplicate id ") + command.id);

                for (auto& press : command.defaults)
                    expect (keys::Bindings::get().conflictFor (command.id, press).isEmpty(),
                            juce::String (command.id) + ": " + press.getTextDescription() + " is also "
                              + keys::Bindings::get().conflictFor (command.id, press));
            }
        }

        beginTest ("the user's keys replace the defaults, persist, and reset");
        {
            juce::PropertySet settings;
            auto& bindings = keys::Bindings::get();
            bindings.load (settings);

            expect (bindings.matches ("view.edit", juce::KeyPress ('e')));
            bindings.set ("view.edit", { juce::KeyPress ('w', juce::ModifierKeys::ctrlModifier, 0) });
            expect (! bindings.matches ("view.edit", juce::KeyPress ('e')));
            expect (bindings.matches ("view.edit", juce::KeyPress ('w', juce::ModifierKeys::ctrlModifier, 0)));

            bindings.load (settings);   // read back from the settings
            expect (bindings.matches ("view.edit", juce::KeyPress ('W', juce::ModifierKeys::ctrlModifier, 0)));
            expect (! bindings.isDefault ("view.edit"));

            bindings.reset ("view.edit");
            expect (bindings.isDefault ("view.edit"));
            expect (bindings.matches ("view.edit", juce::KeyPress ('e')));

            // Shift makes a different key: "." and Shift+"." are two commands
            expect (bindings.matches ("input.dot", juce::KeyPress ('.')));
            expect (! bindings.matches ("input.dot", juce::KeyPress ('.', juce::ModifierKeys::shiftModifier, 0)));

            static juce::PropertySet empty;
            bindings.load (empty);   // leave the defaults for the other tests (and no dangling settings)
        }
    }
};

static KeyCommandTests keyCommandTests;
