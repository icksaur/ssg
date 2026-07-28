#pragma once

#include <ssg/CommandRegistry.h>
#include <ssg/CommandSpecBuilder.h>
#include <ssg/EditorSession.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ssg {

class CommandCatalog;

class EditorSessionBuilder {
public:
    EditorSessionBuilder();
    ~EditorSessionBuilder();

    EditorSessionBuilder(EditorSessionBuilder const&) = delete;
    EditorSessionBuilder& operator=(EditorSessionBuilder const&) = delete;
    EditorSessionBuilder(EditorSessionBuilder&&) noexcept;
    EditorSessionBuilder& operator=(EditorSessionBuilder&&) noexcept;

    // Registers a command: its declaration and its implementation together.
    EditorSessionBuilder& add(CommandSpecBuilder spec);

    EditorSessionBuilder& services(CommandServices& services) noexcept;
    [[nodiscard]] std::shared_ptr<CommandCatalog> catalog() const;
    [[nodiscard]] std::unique_ptr<EditorSession> build();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ssg
