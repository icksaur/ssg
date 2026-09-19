#pragma once

#include <ssg/Editor.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace ssg::test {

CommandResult requireCommand(ClientInputResult result);
PromptEditState promptText(std::string text);
CommandResult dispatchInput(Editor& editor, ClientInput input);
CommandResult openFile(Editor& editor, std::string_view path);
CommandResult typeText(Editor& editor, std::string text);
CommandResult setSelections(
    Editor& editor,
    std::initializer_list<std::pair<std::uint64_t, std::uint64_t>> ranges);
CommandResult clickDocument(Editor& editor, std::uint64_t byteOffset,
                            bool additive = false,
                            bool selectWord = false);
CommandResult dragDocument(Editor& editor, std::uint64_t anchor,
                           std::uint64_t active,
                           bool additive = false);

}
