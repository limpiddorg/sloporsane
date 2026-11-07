#ifndef SOS_PRIVATE_COPY_HPP
#define SOS_PRIVATE_COPY_HPP

#include <filesystem>

// Destination must not exist. Never uses hard links or follows symbolic links.
// Returns true when the native copy-on-write optimization was used.
bool copyPrivateFile(const std::filesystem::path& source, const std::filesystem::path& destination,
                     bool allowClone = true);

#endif // SOS_PRIVATE_COPY_HPP
