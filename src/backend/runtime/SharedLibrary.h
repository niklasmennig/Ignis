#pragma once

#include "IG_Config.h"

namespace IG {
class SharedLibrary {
public:
    SharedLibrary() = default;
    explicit SharedLibrary(const std::filesystem::path& file);
    ~SharedLibrary() = default;

    void* symbol(const std::string_view& name) const;
    void unload();

    inline operator bool() const { return mInternal != nullptr; }
    inline const std::filesystem::path& path() const { return mPath; }

private:
    std::filesystem::path mPath;
    std::shared_ptr<class SharedLibraryInternal> mInternal;
};
} // namespace IG