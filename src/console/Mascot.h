#pragma once
#include <string>
#include <vector>

namespace cheburnet {

struct MascotEye {
    int x;
    int y; // cell offset from the mascot's top-left origin
};

struct MascotArt {
    std::vector<std::wstring> lines;
    wchar_t                   starMarker; // glyph painted red (the cap star)
    int                       width;      // nominal width in columns

    // Idle-animation metadata (cosmetic only).
    std::vector<MascotEye>    eyes;             // eye cells for blink/glint
    bool                      blinkSwap = false; // swap open<->closed glyph on blink
    bool                      glint = false;     // overlay a bright sparkle on blink
    wchar_t                   eyeOpen = L'o';
    wchar_t                   eyeClosed = L'-';
};

// Full-size mascot (fits a standard 100-120 column window).
const MascotArt& MascotFull();

// Compact mascot for narrow windows.
const MascotArt& MascotCompact();

// Choose full or compact based on available console width.
const MascotArt& MascotForWidth(int consoleWidth);

} // namespace cheburnet
