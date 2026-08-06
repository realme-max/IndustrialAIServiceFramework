#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "iaisf/plugin/detail/plugin_safe_path.hpp"

namespace iaisf::plugin::detail {
namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{IAISF_TEST_FIXTURE_PATH};
}

TEST(SafePathResolverTest, AcceptsValidRelativeLibrary) {
    const auto fixture = fixture_path();
    auto result = SafePathResolver::resolve(
        fixture.parent_path(), fixture.filename());
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), std::filesystem::canonical(fixture));
}

TEST(SafePathResolverTest, RejectsAbsolutePath) {
    const auto fixture = fixture_path();
    auto result = SafePathResolver::resolve(
        fixture.parent_path(), fixture);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, RejectsParentAndDotComponents) {
    const auto root = fixture_path().parent_path();
    auto parent = SafePathResolver::resolve(
        root, std::filesystem::path{"../"} / fixture_path().filename());
    ASSERT_FALSE(parent);
    EXPECT_EQ(parent.error().code, ErrorCode::InvalidArgument);

    auto dot = SafePathResolver::resolve(
        root, std::filesystem::path{"."} / fixture_path().filename());
    ASSERT_FALSE(dot);
    EXPECT_EQ(dot.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, RejectsControlCharactersAndBackslashes) {
    const auto root = fixture_path().parent_path();
    auto control = SafePathResolver::resolve(
        root, std::filesystem::path{std::string{"bad\x01.so"}});
    ASSERT_FALSE(control);
    EXPECT_EQ(control.error().code, ErrorCode::InvalidArgument);

    auto backslash = SafePathResolver::resolve(
        root, std::filesystem::path{"nested\\fixture.so"});
    ASSERT_FALSE(backslash);
    EXPECT_EQ(backslash.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, RejectsDriveAndUncStylePaths) {
    const auto root = fixture_path().parent_path();
    auto drive = SafePathResolver::resolve(
        root, std::filesystem::path{"C:\\plugins\\fixture.so"});
    ASSERT_FALSE(drive);
    EXPECT_EQ(drive.error().code, ErrorCode::InvalidArgument);

    auto unc = SafePathResolver::resolve(
        root, std::filesystem::path{"\\\\server\\share\\fixture.so"});
    ASSERT_FALSE(unc);
    EXPECT_EQ(unc.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, RejectsMissingRootAndLibrary) {
    const auto fixture = fixture_path();
    auto missing_root = SafePathResolver::canonical_root(
        fixture.parent_path() / "missing-root");
    ASSERT_FALSE(missing_root);
    EXPECT_EQ(missing_root.error().code, ErrorCode::NotFound);

    auto missing_library = SafePathResolver::resolve(
        fixture.parent_path(), "missing-library.so");
    ASSERT_FALSE(missing_library);
    EXPECT_EQ(missing_library.error().code, ErrorCode::NotFound);
}

TEST(SafePathResolverTest, RejectsSymlinkRootWhenSupported) {
    const auto fixture = fixture_path();
    const auto link_root =
        std::filesystem::temp_directory_path() / "iaisf_plugin_root_link";
    std::error_code error;
    std::filesystem::remove(link_root, error);
    std::filesystem::create_directory_symlink(
        fixture.parent_path(), link_root, error);
    if (error) {
        GTEST_SKIP() << "symlink creation is unavailable";
    }
    auto result = SafePathResolver::canonical_root(link_root);
    std::filesystem::remove(link_root, error);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, RejectsSymlinkLibraryWhenSupported) {
    const auto fixture = fixture_path();
    const auto link_library =
        fixture.parent_path() / "iaisf_plugin_library_link.so";
    std::error_code error;
    std::filesystem::remove(link_library, error);
    std::filesystem::create_symlink(fixture, link_library, error);
    if (error) {
        GTEST_SKIP() << "symlink creation is unavailable";
    }
    auto result = SafePathResolver::resolve(
        fixture.parent_path(), link_library.filename());
    std::filesystem::remove(link_library, error);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(SafePathResolverTest, PermissionFailureIsExplicitlyHandled) {
#if defined(_WIN32)
    GTEST_SKIP() << "permission fixture requires an ACL-specific test host";
#else
    const auto blocked =
        std::filesystem::temp_directory_path() / "iaisf_plugin_permission_root";
    std::error_code error;
    std::filesystem::remove_all(blocked, error);
    std::filesystem::create_directory(blocked, error);
    ASSERT_FALSE(error);
    std::filesystem::permissions(
        blocked, std::filesystem::perms::none,
        std::filesystem::perm_options::replace, error);
    ASSERT_FALSE(error);
    auto result = SafePathResolver::canonical_root(blocked);
    std::filesystem::permissions(
        blocked, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
    std::filesystem::remove_all(blocked, error);
    if (result) {
        GTEST_SKIP() << "test process retains permission despite restricted fixture";
    }
    EXPECT_TRUE(result.error().code == ErrorCode::InvalidArgument ||
                result.error().code == ErrorCode::NotFound ||
                result.error().code == ErrorCode::SystemError);
#endif
}

}  // namespace
}  // namespace iaisf::plugin::detail
