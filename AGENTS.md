# SSG
SSG is a C++ TUI text editor. There is one process, one client, and one screen.

# goals
VSCode-like mouse and keyboard non-modal interaction.
Human-readable code.
All actions available via keyboard.

## Code architecture
The main() function should instantiate components and start threads and do simple loops.
The architecture is as simple and flat as possible.  Prefer broad and "leafy" composition.
Avoid deep ownership hierarchies.  Prefer minimal layering.
Prefer composition to inheritance.  Inherit from pure C++ interfaces when required for polymorphism.
Minimize unique data structures and give them more functionality that can be tested.
Deleting a concept beats generalizing it.

## process
Read the code you are changing, then change it.  Git makes the change itself low-risk.
Always compile and test code changes.

```sh
cmake --build build     # cmake --preset dev to configure the first time
ctest --preset dev      # fast loop, ~8s; ctest --preset all adds recovery and theme
```

## values
The shortest trustworthy dev loop wins.
Less is more.
Design and the compiler guarantee correctness.

## comments
Lift all comments into better names.
Rarely comment "why" code exists.
Short three-line max comments for classes.

## structure
/src program sources
/include headers
/tests test code
