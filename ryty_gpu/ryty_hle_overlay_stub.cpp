// The in-game Controllers window lives in libSceVideoOut; in the other host libraries the
// settings hotkey has nothing to open.
namespace Ryty {
void ToggleControllerOverlay() {}
}

// FEAT-GT7-185: the shared system-service stubs ask whether the shader precompile holds the game
// on the splash screen; only libSceVideoOut compiles shaders, so here it never does.
namespace Vulkan {
bool PrecompileHoldsGame() {
    return false;
}
} // namespace Vulkan
