#include <gtest/gtest.h>
#include "credentials_parser.h"
#include <cstdlib>

#include <sstream>

// ============================================================================
// parse_aws_credential_profiles Tests
// ============================================================================

TEST(ParseAwsCredentialProfilesTest, EmptyInput) {
    std::istringstream input("");
    auto profiles = parse_aws_credential_profiles(input);
    EXPECT_TRUE(profiles.empty());
}

TEST(ParseAwsCredentialProfilesTest, SingleProfile) {
    std::istringstream input(
        "[default]\n"
        "aws_access_key_id = AKIA...\n"
        "aws_secret_access_key = ...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, MultipleProfiles) {
    std::istringstream input(
        "[default]\n"
        "aws_access_key_id = AKIA...\n"
        "aws_secret_access_key = ...\n"
        "\n"
        "[production]\n"
        "aws_access_key_id = AKIA...\n"
        "aws_secret_access_key = ...\n"
        "\n"
        "[staging]\n"
        "aws_access_key_id = AKIA...\n"
        "aws_secret_access_key = ...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 3u);
    EXPECT_EQ(profiles[0], "default");
    EXPECT_EQ(profiles[1], "production");
    EXPECT_EQ(profiles[2], "staging");
}

TEST(ParseAwsCredentialProfilesTest, ProfileWithHyphen) {
    std::istringstream input(
        "[my-profile-name]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "my-profile-name");
}

TEST(ParseAwsCredentialProfilesTest, ProfileWithUnderscore) {
    std::istringstream input(
        "[my_profile_name]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "my_profile_name");
}

TEST(ParseAwsCredentialProfilesTest, ProfileWithNumbers) {
    std::istringstream input(
        "[profile123]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "profile123");
}

TEST(ParseAwsCredentialProfilesTest, WhitespaceAroundBrackets) {
    std::istringstream input(
        "  [default]  \n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, WhitespaceInsideBrackets) {
    std::istringstream input(
        "[ default ]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, EmptyBrackets) {
    std::istringstream input(
        "[]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    EXPECT_TRUE(profiles.empty());
}

TEST(ParseAwsCredentialProfilesTest, BracketsWithOnlyWhitespace) {
    std::istringstream input(
        "[   ]\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    EXPECT_TRUE(profiles.empty());
}

TEST(ParseAwsCredentialProfilesTest, CommentsIgnored) {
    std::istringstream input(
        "# This is a comment\n"
        "[default]\n"
        "# Another comment\n"
        "aws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, BlankLinesIgnored) {
    std::istringstream input(
        "\n"
        "\n"
        "[default]\n"
        "\n"
        "aws_access_key_id = AKIA...\n"
        "\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, WindowsLineEndings) {
    std::istringstream input(
        "[default]\r\n"
        "aws_access_key_id = AKIA...\r\n"
        "[production]\r\n"
        "aws_access_key_id = AKIA...\r\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(profiles[0], "default");
    EXPECT_EQ(profiles[1], "production");
}

TEST(ParseAwsCredentialProfilesTest, TabIndentation) {
    std::istringstream input(
        "\t[default]\n"
        "\taws_access_key_id = AKIA...\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 1u);
    EXPECT_EQ(profiles[0], "default");
}

TEST(ParseAwsCredentialProfilesTest, NoKeyValuePairs) {
    // Profile header with no content
    std::istringstream input(
        "[default]\n"
        "[another]\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(profiles[0], "default");
    EXPECT_EQ(profiles[1], "another");
}

TEST(ParseAwsCredentialProfilesTest, PreservesOrder) {
    std::istringstream input(
        "[zebra]\n"
        "[alpha]\n"
        "[middle]\n"
    );
    auto profiles = parse_aws_credential_profiles(input);
    ASSERT_EQ(profiles.size(), 3u);
    EXPECT_EQ(profiles[0], "zebra");
    EXPECT_EQ(profiles[1], "alpha");
    EXPECT_EQ(profiles[2], "middle");
}

// ============================================================================
// CredentialsListState Tests
// ============================================================================

TEST(CredentialsListStateTest, DefaultConstruction) {
    CredentialsListState state;
    EXPECT_EQ(state.state, CredentialsListState::LoadState::Idle);
    EXPECT_TRUE(state.profiles.empty());
    EXPECT_TRUE(state.error_message.empty());
    EXPECT_EQ(state.selected_index[0], -1);
    EXPECT_EQ(state.selected_index[1], -1);
}

TEST(CredentialsListStateTest, HasValidSelectionInvalidIndex) {
    CredentialsListState state;
    state.set_profiles({"default", "production"});

    EXPECT_FALSE(state.has_valid_selection(-1));
    EXPECT_FALSE(state.has_valid_selection(2));
    EXPECT_FALSE(state.has_valid_selection(0));  // No selection yet
    EXPECT_FALSE(state.has_valid_selection(1));
}

TEST(CredentialsListStateTest, HasValidSelectionValidIndex) {
    CredentialsListState state;
    state.set_profiles({"default", "production", "staging"});
    state.selected_index[0] = 0;
    state.selected_index[1] = 2;

    EXPECT_TRUE(state.has_valid_selection(0));
    EXPECT_TRUE(state.has_valid_selection(1));
}

TEST(CredentialsListStateTest, GetSelectedProfile) {
    CredentialsListState state;
    state.set_profiles({"default", "production", "staging"});
    state.selected_index[0] = 1;
    state.selected_index[1] = 0;

    EXPECT_EQ(state.get_selected_profile(0), "production");
    EXPECT_EQ(state.get_selected_profile(1), "default");
}

TEST(CredentialsListStateTest, GetSelectedProfileNoSelection) {
    CredentialsListState state;
    state.set_profiles({"default", "production"});

    EXPECT_EQ(state.get_selected_profile(0), "");
    EXPECT_EQ(state.get_selected_profile(1), "");
}

TEST(CredentialsListStateTest, Reset) {
    CredentialsListState state;
    state.set_profiles({"default", "production"});
    state.selected_index[0] = 1;
    state.selected_index[1] = 0;

    state.reset();

    EXPECT_EQ(state.state, CredentialsListState::LoadState::Idle);
    EXPECT_TRUE(state.profiles.empty());
    EXPECT_TRUE(state.error_message.empty());
    EXPECT_EQ(state.selected_index[0], -1);
    EXPECT_EQ(state.selected_index[1], -1);
}

TEST(CredentialsListStateTest, SetProfiles) {
    CredentialsListState state;
    state.state = CredentialsListState::LoadState::Loading;
    state.error_message = "previous error";

    state.set_profiles({"default", "production"});

    EXPECT_EQ(state.state, CredentialsListState::LoadState::Loaded);
    ASSERT_EQ(state.profiles.size(), 2u);
    EXPECT_EQ(state.profiles[0], "default");
    EXPECT_EQ(state.profiles[1], "production");
    EXPECT_TRUE(state.error_message.empty());
}

TEST(CredentialsListStateTest, SetError) {
    CredentialsListState state;
    state.set_profiles({"default"});

    state.set_error("File not found");

    EXPECT_EQ(state.state, CredentialsListState::LoadState::Error);
    EXPECT_EQ(state.error_message, "File not found");
    EXPECT_TRUE(state.profiles.empty());
}

TEST(CredentialsListStateTest, FindProfileIndex) {
    CredentialsListState state;
    state.set_profiles({"default", "production", "staging"});

    EXPECT_EQ(state.find_profile_index("default"), 0);
    EXPECT_EQ(state.find_profile_index("production"), 1);
    EXPECT_EQ(state.find_profile_index("staging"), 2);
    EXPECT_EQ(state.find_profile_index("nonexistent"), -1);
}

TEST(CredentialsListStateTest, FindProfileIndexEmpty) {
    CredentialsListState state;
    EXPECT_EQ(state.find_profile_index("default"), -1);
}

TEST(CredentialsListStateTest, LoadStateTransitions) {
    CredentialsListState state;

    EXPECT_EQ(state.state, CredentialsListState::LoadState::Idle);

    state.state = CredentialsListState::LoadState::Loading;
    EXPECT_EQ(state.state, CredentialsListState::LoadState::Loading);

    state.set_profiles({"default"});
    EXPECT_EQ(state.state, CredentialsListState::LoadState::Loaded);

    state.reset();
    EXPECT_EQ(state.state, CredentialsListState::LoadState::Idle);

    state.state = CredentialsListState::LoadState::Loading;
    state.set_error("Error");
    EXPECT_EQ(state.state, CredentialsListState::LoadState::Error);
}

// ============================================================================
// get_aws_credentials_path Tests
// ============================================================================

TEST(GetAwsCredentialsPathTest, ReturnsNonEmptyPath) {
    // This test assumes the test environment has HOME or USERPROFILE set
    std::string path = get_aws_credentials_path();
    // Path might be empty in some CI environments, but if HOME is set, it should work
    if (std::getenv("HOME") || std::getenv("USERPROFILE")) {
        EXPECT_FALSE(path.empty());
        EXPECT_TRUE(path.find(".aws/credentials") != std::string::npos ||
                    path.find(".aws\\credentials") != std::string::npos);
    }
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(CredentialsIntegrationTest, ParseAndSelectProfile) {
    std::istringstream input(
        "[default]\n"
        "aws_access_key_id = AKIA...\n"
        "\n"
        "[production]\n"
        "aws_access_key_id = AKIA...\n"
    );

    auto profiles = parse_aws_credential_profiles(input);

    CredentialsListState state;
    state.set_profiles(profiles);

    // Find and select "production" profile
    int idx = state.find_profile_index("production");
    ASSERT_GE(idx, 0);
    state.selected_index[0] = idx;

    EXPECT_TRUE(state.has_valid_selection(0));
    EXPECT_EQ(state.get_selected_profile(0), "production");
}

TEST(CredentialsIntegrationTest, RealWorldCredentialsFile) {
    // Simulate a typical AWS credentials file
    std::istringstream input(
        "# AWS credentials file\n"
        "\n"
        "[default]\n"
        "aws_access_key_id = AKIAIOSFODNN7EXAMPLE\n"
        "aws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY\n"
        "\n"
        "[dev-account]\n"
        "aws_access_key_id = AKIAI44QH8DHBEXAMPLE\n"
        "aws_secret_access_key = je7MtGbClwBF/2Zp9Utk/h3yCo8nvbEXAMPLEKEY\n"
        "region = us-west-2\n"
        "\n"
        "[prod-account]\n"
        "aws_access_key_id = AKIAI44QH8DHBEXAMPLE\n"
        "aws_secret_access_key = je7MtGbClwBF/2Zp9Utk/h3yCo8nvbEXAMPLEKEY\n"
        "region = eu-west-1\n"
        "\n"
        "[cross-account-role]\n"
        "role_arn = arn:aws:iam::123456789012:role/role-name\n"
        "source_profile = default\n"
    );

    auto profiles = parse_aws_credential_profiles(input);

    ASSERT_EQ(profiles.size(), 4u);
    EXPECT_EQ(profiles[0], "default");
    EXPECT_EQ(profiles[1], "dev-account");
    EXPECT_EQ(profiles[2], "prod-account");
    EXPECT_EQ(profiles[3], "cross-account-role");
}

// ============================================================================
// Credentials file location
// ============================================================================

TEST(CredentialsPathTest, ExplicitEnvironmentOverrideWins) {
    const char* prev = std::getenv("AWS_SHARED_CREDENTIALS_FILE");
    std::string saved = prev ? prev : "";
    ::setenv("AWS_SHARED_CREDENTIALS_FILE", "/custom/creds", 1);
    EXPECT_EQ(get_aws_credentials_path(), "/custom/creds");
    if (saved.empty()) ::unsetenv("AWS_SHARED_CREDENTIALS_FILE");
    else ::setenv("AWS_SHARED_CREDENTIALS_FILE", saved.c_str(), 1);
}

TEST(CredentialsPathTest, FallsBackToTheHomeDirectory) {
    const char* prev = std::getenv("AWS_SHARED_CREDENTIALS_FILE");
    std::string saved = prev ? prev : "";
    ::unsetenv("AWS_SHARED_CREDENTIALS_FILE");
    std::string path = get_aws_credentials_path();
    // Either a HOME-derived path, or empty when HOME is unset.
    if (!path.empty()) {
        EXPECT_NE(path.find(".aws/credentials"), std::string::npos) << path;
    }
    if (!saved.empty()) ::setenv("AWS_SHARED_CREDENTIALS_FILE", saved.c_str(), 1);
}
