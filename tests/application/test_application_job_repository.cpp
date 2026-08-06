#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "iaisf/application/in_memory_application_job_repository.hpp"

namespace iaisf::application {

class ApplicationJobRepositoryTestAccess {
public:
    static void set_version(
        InMemoryApplicationJobRepository& repository,
        const ApplicationJobId& id,
        const std::uint64_t version) {
        std::lock_guard<std::mutex> lock(repository.mutex_);
        const auto found = repository.records_.find(id);
        if (found == repository.records_.end()) {
            throw std::logic_error{"test job not found"};
        }
        found->second.version_ = version;
    }
};

namespace {

[[nodiscard]] ApplicationJobId job_id(const std::string& text) {
    auto parsed = ApplicationJobId::parse(text);
    return std::move(parsed).value();
}

[[nodiscard]] ApplicationJobId syntactically_invalid_job_id() {
    auto result = ApplicationJobId::parse("job-invalid-boundary");
    auto id = std::move(result).value();
    // Deliberately violate the public value through its non-owning view to
    // exercise defense-in-depth at every Repository boundary. Production has
    // no unchecked constructor or test-only mutation hook.
    auto* bytes = const_cast<char*>(id.value().data());
    bytes[0] = '/';
    return id;
}

[[nodiscard]] ArtifactRef artifact(const std::string& id = "input-1") {
    return ArtifactRef{
        id,
        std::string(kSha256HexBytes, 'b'),
        256U,
        "point_cloud",
        "application/vnd.iaisf.pointcloud.xyz-f32le",
        std::string{"workpiece"},
        std::string{"mm"},
        8U,
    };
}

[[nodiscard]] ApplicationJobCreateRequest request(
    const std::string& id,
    const IndustrialApplication application =
        IndustrialApplication::WeldInspection,
    const ScenePhase phase = ScenePhase::PostWeld) {
    return ApplicationJobCreateRequest{
        job_id(id),
        application,
        phase,
        ApplicationJobTimePoint{std::chrono::seconds{100}},
        {artifact()},
    };
}

[[nodiscard]] std::unique_ptr<InMemoryApplicationJobRepository> repository(
    const std::size_t capacity = 8U) {
    auto made = InMemoryApplicationJobRepository::make(capacity);
    return std::move(made).value();
}

template <typename T>
void expect_failure(
    const ApplicationRepositoryResult<T>& result,
    const ApplicationRepositoryFailure category) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, category);
    EXPECT_FALSE(result.error().detail.message.empty());
    EXPECT_LE(result.error().detail.message.size(), 128U);
}

class StartGate {
public:
    StartGate() : future_(promise_.get_future().share()) {}

    void wait() const {
        future_.wait();
    }

    void release() {
        promise_.set_value();
    }

private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
};

TEST(ApplicationRepositoryFailureTest, UsesStableStructuredNames) {
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::InvalidArgument), "invalid_argument");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::DuplicateId), "duplicate_id");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::NotFound), "not_found");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::CapacityExceeded), "capacity_exceeded");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::VersionConflict), "version_conflict");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::VersionExhausted), "version_exhausted");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::InvalidTransition), "invalid_transition");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::InvalidTimestamp), "invalid_timestamp");
    EXPECT_EQ(to_string(ApplicationRepositoryFailure::InternalFailure), "internal_failure");
    EXPECT_EQ(
        to_string(static_cast<ApplicationRepositoryFailure>(100)),
        "unknown");
}

TEST(InMemoryApplicationJobRepositoryTest, RejectsZeroCapacity) {
    const auto made = InMemoryApplicationJobRepository::make(0U);
    expect_failure(made, ApplicationRepositoryFailure::InvalidArgument);
}

TEST(InMemoryApplicationJobRepositoryTest, CreatesAcceptedVersionOneSnapshot) {
    auto repo = repository();
    const auto created = repo->create(request("job-create"));
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value().state(), ApplicationJobState::Accepted);
    EXPECT_EQ(created.value().version(), 1U);
    EXPECT_EQ(created.value().created_at(), created.value().updated_at());
    EXPECT_EQ(repo->size(), 1U);
    EXPECT_EQ(repo->capacity(), 8U);
}

TEST(InMemoryApplicationJobRepositoryTest, InvalidCreateIsTransactional) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-existing")));
    const auto before = repo->get(
        job_id("job-existing"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(before);

    auto invalid_pair = request(
        "job-invalid-pair",
        IndustrialApplication::WeldInspection,
        ScenePhase::PreWeld);
    expect_failure(
        repo->create(invalid_pair),
        ApplicationRepositoryFailure::InvalidArgument);

    auto invalid_artifact = request("job-invalid-artifact");
    invalid_artifact.input_artifacts.front().artifact_id = "../secret";
    expect_failure(
        repo->create(invalid_artifact),
        ApplicationRepositoryFailure::InvalidArgument);
    EXPECT_EQ(repo->size(), 1U);
    const auto after = repo->get(
        job_id("job-existing"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().version(), before.value().version());
    EXPECT_EQ(after.value().state(), before.value().state());
}

TEST(InMemoryApplicationJobRepositoryTest, InvalidIdFailsClosedAtEveryBoundary) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-stable")));
    const auto before = repo->get(
        job_id("job-stable"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(before);

    const auto invalid_id = syntactically_invalid_job_id();
    ASSERT_FALSE(invalid_id.valid());
    auto invalid_request = request("job-temporary");
    invalid_request.job_id = invalid_id;

    expect_failure(
        repo->create(invalid_request),
        ApplicationRepositoryFailure::InvalidArgument);
    expect_failure(
        repo->get(invalid_id, IndustrialApplication::WeldInspection),
        ApplicationRepositoryFailure::InvalidArgument);
    expect_failure(
        repo->transition(
            invalid_id,
            IndustrialApplication::WeldInspection,
            1U,
            ApplicationJobState::Queued,
            ApplicationJobTimePoint{std::chrono::seconds{101}}),
        ApplicationRepositoryFailure::InvalidArgument);
    expect_failure(
        repo->erase_terminal(
            invalid_id, IndustrialApplication::WeldInspection, 1U),
        ApplicationRepositoryFailure::InvalidArgument);

    EXPECT_EQ(repo->size(), 1U);
    const auto after = repo->get(
        job_id("job-stable"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().state(), before.value().state());
    EXPECT_EQ(after.value().version(), before.value().version());
    EXPECT_EQ(after.value().created_at(), before.value().created_at());
    EXPECT_EQ(after.value().updated_at(), before.value().updated_at());
}

TEST(InMemoryApplicationJobRepositoryTest, DuplicateAndCapacityFailuresDoNotMutate) {
    auto repo = repository(1U);
    ASSERT_TRUE(repo->create(request("job-one")));
    expect_failure(
        repo->create(request("job-one")),
        ApplicationRepositoryFailure::DuplicateId);
    expect_failure(
        repo->create(request("job-two")),
        ApplicationRepositoryFailure::CapacityExceeded);
    EXPECT_EQ(repo->size(), 1U);
}

TEST(InMemoryApplicationJobRepositoryTest, GetDistinguishesNotFoundWithoutCrossAppLeak) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-isolated")));
    expect_failure(
        repo->get(
            job_id("missing"), IndustrialApplication::WeldInspection),
        ApplicationRepositoryFailure::NotFound);
    expect_failure(
        repo->get(
            job_id("job-isolated"),
            IndustrialApplication::WeldingGuidance),
        ApplicationRepositoryFailure::NotFound);
}

TEST(InMemoryApplicationJobRepositoryTest, ReturnedSnapshotsDoNotAliasStorage) {
    auto repo = repository();
    auto source = request("job-copy");
    ASSERT_TRUE(repo->create(source));
    source.input_artifacts.front().artifact_id = "changed-source";

    auto first = repo->get(
        job_id("job-copy"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(first);
    auto caller_copy = first.value().input_artifacts();
    caller_copy.front().artifact_id = "changed-result";

    const auto second = repo->get(
        job_id("job-copy"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(second);
    EXPECT_EQ(
        second.value().input_artifacts().front().artifact_id,
        "input-1");
}

TEST(InMemoryApplicationJobRepositoryTest, TransitionIsAtomicAndIncrementsVersionOnce) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-transition")));
    const auto updated_at =
        ApplicationJobTimePoint{std::chrono::seconds{101}};
    const auto transitioned = repo->transition(
        job_id("job-transition"),
        IndustrialApplication::WeldInspection,
        1U,
        ApplicationJobState::Queued,
        updated_at);
    ASSERT_TRUE(transitioned);
    EXPECT_EQ(transitioned.value().state(), ApplicationJobState::Queued);
    EXPECT_EQ(transitioned.value().version(), 2U);
    EXPECT_EQ(transitioned.value().updated_at(), updated_at);
}

TEST(InMemoryApplicationJobRepositoryTest, FailedTransitionsLeaveSnapshotUnchanged) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-unchanged")));
    const auto before = repo->get(
        job_id("job-unchanged"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(before);

    expect_failure(
        repo->transition(
            job_id("job-unchanged"),
            IndustrialApplication::WeldInspection,
            2U,
            ApplicationJobState::Queued,
            ApplicationJobTimePoint{std::chrono::seconds{101}}),
        ApplicationRepositoryFailure::VersionConflict);
    expect_failure(
        repo->transition(
            job_id("job-unchanged"),
            IndustrialApplication::WeldInspection,
            1U,
            ApplicationJobState::Running,
            ApplicationJobTimePoint{std::chrono::seconds{101}}),
        ApplicationRepositoryFailure::InvalidTransition);
    expect_failure(
        repo->transition(
            job_id("job-unchanged"),
            IndustrialApplication::WeldInspection,
            1U,
            ApplicationJobState::Queued,
            ApplicationJobTimePoint{std::chrono::seconds{99}}),
        ApplicationRepositoryFailure::InvalidTimestamp);

    const auto after = repo->get(
        job_id("job-unchanged"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().version(), before.value().version());
    EXPECT_EQ(after.value().state(), before.value().state());
    EXPECT_EQ(after.value().updated_at(), before.value().updated_at());
}

TEST(InMemoryApplicationJobRepositoryTest, InspectionWaitingHumanIsRejected) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-inspection")));
    ASSERT_TRUE(repo->transition(
        job_id("job-inspection"), IndustrialApplication::WeldInspection,
        1U, ApplicationJobState::Queued,
        ApplicationJobTimePoint{std::chrono::seconds{101}}));
    ASSERT_TRUE(repo->transition(
        job_id("job-inspection"), IndustrialApplication::WeldInspection,
        2U, ApplicationJobState::Dispatching,
        ApplicationJobTimePoint{std::chrono::seconds{102}}));
    ASSERT_TRUE(repo->transition(
        job_id("job-inspection"), IndustrialApplication::WeldInspection,
        3U, ApplicationJobState::Running,
        ApplicationJobTimePoint{std::chrono::seconds{103}}));
    expect_failure(
        repo->transition(
            job_id("job-inspection"),
            IndustrialApplication::WeldInspection,
            4U,
            ApplicationJobState::WaitingHuman,
            ApplicationJobTimePoint{std::chrono::seconds{104}}),
        ApplicationRepositoryFailure::InvalidTransition);
}

TEST(InMemoryApplicationJobRepositoryTest, TerminalStateCannotTransition) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-terminal")));
    ASSERT_TRUE(repo->transition(
        job_id("job-terminal"), IndustrialApplication::WeldInspection,
        1U, ApplicationJobState::Cancelled,
        ApplicationJobTimePoint{std::chrono::seconds{101}}));
    expect_failure(
        repo->transition(
            job_id("job-terminal"), IndustrialApplication::WeldInspection,
            2U, ApplicationJobState::Queued,
            ApplicationJobTimePoint{std::chrono::seconds{102}}),
        ApplicationRepositoryFailure::InvalidTransition);
}

TEST(InMemoryApplicationJobRepositoryTest, VersionExhaustionFailsClosed) {
    auto repo = repository();
    const auto id = job_id("job-version-max");
    ASSERT_TRUE(repo->create(request("job-version-max")));
    ApplicationJobRepositoryTestAccess::set_version(
        *repo, id, std::numeric_limits<std::uint64_t>::max());
    expect_failure(
        repo->transition(
            id,
            IndustrialApplication::WeldInspection,
            std::numeric_limits<std::uint64_t>::max(),
            ApplicationJobState::Queued,
            ApplicationJobTimePoint{std::chrono::seconds{101}}),
        ApplicationRepositoryFailure::VersionExhausted);
    const auto unchanged = repo->get(
        id, IndustrialApplication::WeldInspection);
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(
        unchanged.value().version(),
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(unchanged.value().state(), ApplicationJobState::Accepted);
}

TEST(InMemoryApplicationJobRepositoryTest, EraseRequiresTerminalAndExactVersion) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-erase")));
    expect_failure(
        repo->erase_terminal(
            job_id("job-erase"), IndustrialApplication::WeldInspection, 1U),
        ApplicationRepositoryFailure::InvalidTransition);
    ASSERT_TRUE(repo->transition(
        job_id("job-erase"), IndustrialApplication::WeldInspection,
        1U, ApplicationJobState::Cancelled,
        ApplicationJobTimePoint{std::chrono::seconds{101}}));
    expect_failure(
        repo->erase_terminal(
            job_id("job-erase"), IndustrialApplication::WeldInspection, 1U),
        ApplicationRepositoryFailure::VersionConflict);
    const auto erased = repo->erase_terminal(
        job_id("job-erase"), IndustrialApplication::WeldInspection, 2U);
    ASSERT_TRUE(erased);
    EXPECT_EQ(repo->size(), 0U);
    expect_failure(
        repo->erase_terminal(
            job_id("job-erase"), IndustrialApplication::WeldInspection, 2U),
        ApplicationRepositoryFailure::NotFound);
}

TEST(InMemoryApplicationJobRepositoryTest, EraseReleasesCapacityOnlyForMetadata) {
    auto repo = repository(1U);
    ASSERT_TRUE(repo->create(request("job-old")));
    ASSERT_TRUE(repo->transition(
        job_id("job-old"), IndustrialApplication::WeldInspection,
        1U, ApplicationJobState::Cancelled,
        ApplicationJobTimePoint{std::chrono::seconds{101}}));
    ASSERT_TRUE(repo->erase_terminal(
        job_id("job-old"), IndustrialApplication::WeldInspection, 2U));
    EXPECT_TRUE(repo->create(request("job-new")));
    EXPECT_EQ(repo->size(), 1U);
}

TEST(InMemoryApplicationJobRepositoryTest, ConcurrentReadersSeeIndependentSnapshots) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-read")));
    StartGate gate;
    std::atomic<int> successes{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 8; ++index) {
        readers.emplace_back([&repo, &gate, &successes]() {
            gate.wait();
            for (int iteration = 0; iteration < 50; ++iteration) {
                const auto snapshot = repo->get(
                    job_id("job-read"),
                    IndustrialApplication::WeldInspection);
                if (snapshot && snapshot.value().version() == 1U) {
                    ++successes;
                }
            }
        });
    }
    gate.release();
    for (auto& reader : readers) {
        reader.join();
    }
    EXPECT_EQ(successes.load(), 400);
}

TEST(InMemoryApplicationJobRepositoryTest, ConcurrentDistinctCreatesAllSucceed) {
    auto repo = repository(16U);
    StartGate gate;
    std::atomic<int> successes{0};
    std::vector<std::thread> creators;
    for (int index = 0; index < 12; ++index) {
        creators.emplace_back([&repo, &gate, &successes, index]() {
            const auto job = request("job-distinct-" + std::to_string(index));
            gate.wait();
            if (repo->create(job)) {
                ++successes;
            }
        });
    }
    gate.release();
    for (auto& creator : creators) {
        creator.join();
    }
    EXPECT_EQ(successes.load(), 12);
    EXPECT_EQ(repo->size(), 12U);
}

TEST(InMemoryApplicationJobRepositoryTest, ConcurrentSameIdCreatesExactlyOnce) {
    auto repo = repository(16U);
    StartGate gate;
    std::atomic<int> successes{0};
    std::atomic<int> duplicates{0};
    std::vector<std::thread> creators;
    for (int index = 0; index < 12; ++index) {
        creators.emplace_back([&repo, &gate, &successes, &duplicates]() {
            const auto job = request("job-shared");
            gate.wait();
            const auto result = repo->create(job);
            if (result) {
                ++successes;
            } else if (
                result.error().category ==
                ApplicationRepositoryFailure::DuplicateId) {
                ++duplicates;
            }
        });
    }
    gate.release();
    for (auto& creator : creators) {
        creator.join();
    }
    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(duplicates.load(), 11);
    EXPECT_EQ(repo->size(), 1U);
}

TEST(InMemoryApplicationJobRepositoryTest, ConcurrentExpectedVersionTransitionsWinOnce) {
    auto repo = repository();
    ASSERT_TRUE(repo->create(request("job-race")));
    StartGate gate;
    std::atomic<int> successes{0};
    std::atomic<int> conflicts{0};
    std::vector<std::thread> transitions;
    for (int index = 0; index < 12; ++index) {
        transitions.emplace_back([&repo, &gate, &successes, &conflicts]() {
            gate.wait();
            const auto result = repo->transition(
                job_id("job-race"),
                IndustrialApplication::WeldInspection,
                1U,
                ApplicationJobState::Queued,
                ApplicationJobTimePoint{std::chrono::seconds{101}});
            if (result) {
                ++successes;
            } else if (
                result.error().category ==
                ApplicationRepositoryFailure::VersionConflict) {
                ++conflicts;
            }
        });
    }
    gate.release();
    for (auto& transition : transitions) {
        transition.join();
    }
    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(conflicts.load(), 11);
    const auto final = repo->get(
        job_id("job-race"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(final);
    EXPECT_EQ(final.value().state(), ApplicationJobState::Queued);
    EXPECT_EQ(final.value().version(), 2U);
}

}  // namespace
}  // namespace iaisf::application
