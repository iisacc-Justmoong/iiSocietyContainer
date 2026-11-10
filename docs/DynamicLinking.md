# Dynamic linking

FileTree, Maintenance and the optional Qt facade and presentation libraries are
shared targets. iOS configurations use frameworks and WebAssembly uses side
modules. FileTree explicitly links the file provider's temporary lease library;
applications bundle it together with the other required provider libraries.

The Emscripten package configuration enables CMake shared-library imports for side modules. The builder or application must supply the main-module compile and link options.
