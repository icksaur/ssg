# SSG
SSG is a C++ TUI text editor. There is one process, one client, and one screen.

# goals
VSCode-like mouse and keyboard non-modal interaction.
Human-readable code.
All actions available via keyboard.

## Code architecture
The main() function should instantiate components and start threads or do simple loops.
The architecture is as simple and flat as possible.  Prefer broad and "leafy" composition.
Avoid deep ownership hierarchies.
Prefer composition to inheritance.  Inherit from pure C++ interfaces when required.
Minimize unique data structures and give them more functionality that can be tested, like the UI tree.

## process
Jump straight into a change, there is no risk when using git.
Always compile and test code changes.

## values
The shortest trustworthy dev loop wins.
Less is more.
Design guarantees correctness.
Compiler guarantees correctness.

## comments
Lift all comments into better names.
Rarely comment "why" code exists.
Short three-line max comments for classes.