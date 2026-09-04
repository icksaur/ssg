#include <ssg/application.h>
#include <ssg/ssg_terminal.h>

#include <utility>

int main(int argc, char** argv) {
    ssg::app::recordStartupMark("main_entry");
    auto arguments = ssg::app::parseArguments(argc, argv);
    if (arguments.capabilities) return ssg::app::reportCapabilities();
    ssg::app::Application application{std::move(arguments)};
    return application.runEventLoop();
}
