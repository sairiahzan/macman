// remover.hpp — Package Removal Engine [V1.1.0 Patch]
// Handles safe deletion of files, cache cleaning, and the Nuke mechanism.


#pragma once

#include "package.hpp"
#include "database.hpp"
#include <string>
#include <vector>

namespace macman {

class Remover {
public:
    Remover(Database& db);
    ~Remover() = default;

    // Removes a single package cleanly
    bool remove_package(const Package& pkg) const;

    // The Nuke Option: Wipes all packages and cache cleanly
    bool nuke_system() const;
    
private:
    Database& db_;
};

} // namespace macman
