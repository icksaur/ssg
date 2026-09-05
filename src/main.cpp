#include <ssg/application.h>

#include <utility>

int main(int argc, char** argv) {
    ssg::app::recordStartupMark("main_entry");
    auto arguments = ssg::app::parseArguments(argc, argv);
    ssg::app::Application application{std::move(arguments)};
    return application.runEventLoop();
}
