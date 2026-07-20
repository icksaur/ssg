#pragma once

#include <ssg/CommandRegistry.h>
#include <ssg/EditorSession.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

[[nodiscard]] std::vector<CommandDescriptor> p0CommandDescriptors();

class EditorSessionBuilder {
public:
    EditorSessionBuilder();
    ~EditorSessionBuilder();

    EditorSessionBuilder(EditorSessionBuilder const&) = delete;
    EditorSessionBuilder& operator=(EditorSessionBuilder const&) = delete;
    EditorSessionBuilder(EditorSessionBuilder&&) noexcept;
    EditorSessionBuilder& operator=(EditorSessionBuilder&&) noexcept;

    EditorSessionBuilder& bind(std::string commandId,
                               CommandHandler handler);
    EditorSessionBuilder& services(CommandServices& services) noexcept;
    [[nodiscard]] std::unique_ptr<EditorSession> build();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
