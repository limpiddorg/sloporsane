#ifndef SOS_WORKTREE_LISTING_HPP
#define SOS_WORKTREE_LISTING_HPP

#include <string>
#include <vector>

// Decode line-oriented `git worktree list --porcelain`, including Git C quoting.
std::vector<std::string> worktreeListingFields(const std::string& output);

#endif
