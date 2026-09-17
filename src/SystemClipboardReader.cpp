#include <ssg/SystemClipboardReader.h>

#include <utility>

namespace ssg {

std::vector<SystemClipboardProgram> discoverSystemClipboardPrograms(
    const ClipboardEnvironmentLookup& environment,
    const ClipboardExecutableLookup& executable) {
    std::vector<SystemClipboardProgram> programs;
    if (environment("WAYLAND_DISPLAY")) {
        if (auto path = executable("wl-paste")) {
            programs.push_back({std::move(*path), {"--no-newline"}});
        }
    }
    if (environment("DISPLAY")) {
        if (auto path = executable("xclip")) {
            programs.push_back(
                {std::move(*path),
                 {"-selection", "clipboard", "-out", "-target", "UTF8_STRING"}});
        }
    }
    return programs;
}

SystemClipboardPaste planSystemClipboardPaste(const ClientOwnedInput& request,
                                              SystemClipboardRead read) {
    const bool editor =
        request.kind == ClientOwnedInputKind::SystemClipboardPasteIntoEditor;
    const bool text =
        request.kind == ClientOwnedInputKind::SystemClipboardPasteIntoText;
    if (!editor && !text) return {};
    if (read.accepted()) {
        return read.text.empty()
                   ? SystemClipboardPaste{}
                   : SystemClipboardPaste{
                         SystemClipboardPasteKind::CommittedText,
                         std::move(read.text)};
    }
    if (editor) {
        return {SystemClipboardPasteKind::InternalRegister, {}};
    }
    return request.text.empty()
               ? SystemClipboardPaste{}
               : SystemClipboardPaste{SystemClipboardPasteKind::CommittedText,
                                      request.text};
}

} // namespace ssg
