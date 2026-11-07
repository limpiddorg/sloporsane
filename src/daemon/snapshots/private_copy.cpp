#include "private_copy.hpp"
#include <stdexcept>
#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

bool copyPrivateFile(const fs::path& source, const fs::path& destination, bool allowClone) {
    const auto status = fs::symlink_status(source);
    if (fs::exists(fs::symlink_status(destination)))
        throw std::runtime_error("Private copy destination already exists");
    if (fs::is_symlink(status)) {
        fs::copy_symlink(source, destination);
        return false;
    }
    if (!fs::is_regular_file(status)) throw std::runtime_error("Unsupported private copy file type");
    bool cloned = false;
#if defined(__APPLE__)
    if (allowClone) cloned = ::clonefile(source.c_str(), destination.c_str(), CLONE_NOFOLLOW) == 0;
#elif defined(__linux__)
    if (allowClone) {
        const int input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (input < 0) throw std::runtime_error("Cannot open private copy source");
        const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output < 0) {
            ::close(input);
            throw std::runtime_error("Cannot create private copy destination");
        }
        cloned = ::ioctl(output, FICLONE, input) == 0;
        ::close(input);
        ::close(output);
        if (!cloned) fs::remove(destination);
    }
#else
    (void)allowClone; // Including Windows: portable ordinary-copy fallback.
#endif
    if (!cloned && !fs::copy_file(source, destination))
        throw std::runtime_error("Cannot copy private workspace file");
    fs::permissions(destination, status.permissions());
    return cloned;
}
