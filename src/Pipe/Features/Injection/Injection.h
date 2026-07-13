#pragma once

#include "Pipe/PipeTypes.h"

namespace PipeManager::Injection {

    // Migrates configured relative libraries out of the Steam directory.
    void MigrateConfiguredLibraries();

    // Injects the architecture-matched helper library at most once per game process.
    void Apply(const PipeContext& ctx);

} // namespace PipeManager::Injection
