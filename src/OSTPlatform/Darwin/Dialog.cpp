#include "include/Dialog.h"

#include <iostream>

namespace OSTPlatform::Dialog {

void ShowWarning(std::string title, std::string message) {
    std::cerr << "[OpenSteamTool] " << title << ": " << message << '\n';
}

} // namespace OSTPlatform::Dialog
