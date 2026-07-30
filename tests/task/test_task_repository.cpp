#include <array>
#include <atomic>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/task/task_repository.hpp"

namespace iaisf::task {

class TaskRepositoryTestAccess {
public:
    static void set_next_id(
        TaskRepository& repository,
        const std::uint64_t next_id) {
        std::lock_guard<std::mutex> lock(repository.mutex_);
        repository.next_id_ = next_id;
        repository.id_exhausted_ = false;
    }
};

}  // namespace iaisf::task

namespace {

using iaisf::ErrorCode;
using iaisf::make_error;
using iaisf::task::TaskId;
using iaisf::task::TaskLimits;
using iaisf::task::TaskRepository;
using iaisf::task::TaskRequest;
using iaisf::task::TaskState;
using iaisf::task::TransitionOutcome;

TaskLimits default_limits() {
    return TaskLimits::create().value();
}

TaskId create_running(TaskRepository& repository) {
    auto created = repository.create_queued(TaskRequest{"echo", {{"value", 1}}});
    EXPECT_TRUE(created);
    if (!created) {
        return {};
    }
    auto running = repository.mark_running(created.value());
    EXPECT_TRUE(running);
    return created.value();
}

TEST(TaskRepositoryTest, GeneratesMonotonicIdsAndIndependentSnapshots) {
    TaskRepository repository{default_limits()};
    const auto first = repository.create_queued(TaskRequest{"echo", {{"v", 1}}});
    const auto second = repository.create_queued(TaskRequest{"echo", {{"v", 2}}});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().value(), 1U);
    EXPECT_EQ(second.value().value(), 2U);

    ASSERT_TRUE(repository.mark_running(first.value()));
    ASSERT_TRUE(repository.mark_succeeded(first.value(), {{"stored", 7}}));
    auto snapshot = repository.get_snapshot(first.value());
    ASSERT_TRUE(snapshot);
    snapshot.value().operation = "mutated";
    snapshot.value().result->at("stored") = 99;
    auto again = repository.get_snapshot(first.value());
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value().operation, "echo");
    EXPECT_EQ(again.value().result->at("stored"), 7);
}

TEST(TaskRepositoryTest, ConcurrentCreationGeneratesUniqueIds) {
    TaskRepository repository{TaskLimits::create(128).value()};
    std::mutex ids_mutex;
    std::vector<std::uint64_t> ids;
    std::vector<std::thread> creators;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        creators.emplace_back([&, thread_index] {
            for (int index = 0; index < 20; ++index) {
                auto created = repository.create_queued(
                    TaskRequest{"parallel", {{"thread", thread_index}, {"i", index}}});
                if (created) {
                    std::lock_guard<std::mutex> lock(ids_mutex);
                    ids.push_back(created.value().value());
                }
            }
        });
    }
    for (auto& creator : creators) {
        creator.join();
    }

    ASSERT_EQ(ids.size(), 80U);
    EXPECT_EQ(std::set<std::uint64_t>(ids.begin(), ids.end()).size(), ids.size());
}

TEST(TaskRepositoryTest, DoesNotReuseRolledBackIds) {
    TaskRepository repository{default_limits()};
    const auto rolled_back =
        repository.create_queued(TaskRequest{"rollback", {}});
    ASSERT_TRUE(rolled_back);
    ASSERT_TRUE(repository.rollback_queued(rolled_back.value()));

    const auto retained = repository.create_queued(TaskRequest{"retained", {}});

    ASSERT_TRUE(retained);
    EXPECT_EQ(rolled_back.value().value(), 1U);
    EXPECT_EQ(retained.value().value(), 2U);
    EXPECT_EQ(
        repository.get_snapshot(rolled_back.value()).error().code,
        ErrorCode::NotFound);
}

TEST(TaskRepositoryTest, ExhaustedTaskIdsNeverWrapOrReuse) {
    TaskRepository repository{TaskLimits::create(4).value()};
    iaisf::task::TaskRepositoryTestAccess::set_next_id(
        repository,
        std::numeric_limits<std::uint64_t>::max() - 1);

    const auto penultimate =
        repository.create_queued(TaskRequest{"penultimate", {}});
    const auto final = repository.create_queued(TaskRequest{"final", {}});
    const auto exhausted =
        repository.create_queued(TaskRequest{"exhausted", {}});

    ASSERT_TRUE(penultimate);
    ASSERT_TRUE(final);
    EXPECT_EQ(
        penultimate.value().value(),
        std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(final.value().value(), std::numeric_limits<std::uint64_t>::max());
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, ErrorCode::ResourceExhausted);

    ASSERT_TRUE(repository.rollback_queued(penultimate.value()));
    const auto still_exhausted =
        repository.create_queued(TaskRequest{"still-exhausted", {}});
    ASSERT_FALSE(still_exhausted);
    EXPECT_EQ(still_exhausted.error().code, ErrorCode::ResourceExhausted);
}

TEST(TaskRepositoryTest, EnforcesCapacityUntilTerminalRecordIsErased) {
    const auto limits = TaskLimits::create(1).value();
    TaskRepository repository{limits};
    const auto first = repository.create_queued(TaskRequest{"echo", {}});
    ASSERT_TRUE(first);

    auto full = repository.create_queued(TaskRequest{"echo", {}});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, ErrorCode::ResourceExhausted);

    ASSERT_TRUE(repository.mark_running(first.value()));
    ASSERT_TRUE(repository.mark_succeeded(first.value(), {{"ok", true}}));
    ASSERT_TRUE(repository.erase_terminal(first.value()));
    EXPECT_TRUE(repository.create_queued(TaskRequest{"echo", {}}));
}

TEST(TaskRepositoryTest, AppliesLegalSuccessTransitionAndTimestamps) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());

    auto succeeded = repository.mark_succeeded(id, {{"answer", 42}});
    ASSERT_TRUE(succeeded);
    EXPECT_EQ(succeeded.value(), TransitionOutcome::Applied);
    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Succeeded);
    ASSERT_TRUE(snapshot.value().started_at.has_value());
    ASSERT_TRUE(snapshot.value().finished_at.has_value());
    EXPECT_LE(snapshot.value().created_at, *snapshot.value().started_at);
    EXPECT_LE(*snapshot.value().started_at, *snapshot.value().finished_at);
    ASSERT_TRUE(snapshot.value().result.has_value());
    EXPECT_EQ(snapshot.value().result->at("answer"), 42);
}

TEST(TaskRepositoryTest, RejectsIllegalTransitionsWithoutMutation) {
    TaskRepository repository{default_limits()};
    auto created = repository.create_queued(TaskRequest{"echo", {}});
    ASSERT_TRUE(created);

    const auto succeeded = repository.mark_succeeded(created.value(), {});
    const auto failed = repository.mark_failed(
        created.value(), make_error(ErrorCode::InternalError, "failed"));
    const auto timed_out = repository.mark_timed_out(
        created.value(), make_error(ErrorCode::InvalidState, "deadline"));
    ASSERT_FALSE(succeeded);
    ASSERT_FALSE(failed);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(succeeded.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(failed.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(timed_out.error().code, ErrorCode::InvalidState);
    auto snapshot = repository.get_snapshot(created.value());
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Queued);

    ASSERT_TRUE(repository.mark_running(created.value()));
    EXPECT_FALSE(repository.mark_running(created.value()));
}

TEST(TaskRepositoryTest, FirstTerminalTransitionWins) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> applied{0};
    std::vector<std::thread> contenders;
    contenders.emplace_back([&] {
        gate.wait();
        const auto result = repository.mark_succeeded(id, {{"winner", "success"}});
        if (result && result.value() == TransitionOutcome::Applied) {
            applied.fetch_add(1, std::memory_order_relaxed);
        }
    });
    contenders.emplace_back([&] {
        gate.wait();
        const auto result = repository.mark_failed(
            id, make_error(ErrorCode::InternalError, "failed"));
        if (result && result.value() == TransitionOutcome::Applied) {
            applied.fetch_add(1, std::memory_order_relaxed);
        }
    });
    contenders.emplace_back([&] {
        gate.wait();
        const auto result = repository.mark_timed_out(
            id, make_error(ErrorCode::InvalidState, "timed out"));
        if (result && result.value() == TransitionOutcome::Applied) {
            applied.fetch_add(1, std::memory_order_relaxed);
        }
    });
    release.set_value();
    for (auto& contender : contenders) {
        contender.join();
    }

    EXPECT_EQ(applied.load(std::memory_order_relaxed), 1);
    const auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(iaisf::task::is_terminal(snapshot.value().state));
}

TEST(TaskRepositoryTest, RepeatedTerminalRacesHaveExactlyOneAppliedOutcome) {
    for (int round = 0; round < 32; ++round) {
        TaskRepository repository{default_limits()};
        const TaskId id = create_running(repository);
        ASSERT_TRUE(id.valid());
        std::promise<void> release;
        const auto gate = release.get_future().share();
        std::array<std::atomic<int>, 3> outcomes{};
        for (auto& outcome : outcomes) {
            outcome.store(0, std::memory_order_relaxed);
        }
        std::array<std::thread, 3> contenders{
            std::thread{[&] {
                gate.wait();
                const auto result =
                    repository.mark_succeeded(id, {{"round", round}});
                outcomes[0].store(
                    result && result.value() == TransitionOutcome::Applied ? 1
                                                                           : 0,
                    std::memory_order_relaxed);
            }},
            std::thread{[&] {
                gate.wait();
                const auto result = repository.mark_failed(
                    id,
                    make_error(ErrorCode::InternalError, "failed"));
                outcomes[1].store(
                    result && result.value() == TransitionOutcome::Applied ? 1
                                                                           : 0,
                    std::memory_order_relaxed);
            }},
            std::thread{[&] {
                gate.wait();
                const auto result = repository.mark_timed_out(id);
                outcomes[2].store(
                    result && result.value() == TransitionOutcome::Applied ? 1
                                                                           : 0,
                    std::memory_order_relaxed);
            }}};

        release.set_value();
        for (auto& contender : contenders) {
            contender.join();
        }

        const int applied =
            outcomes[0].load(std::memory_order_relaxed) +
            outcomes[1].load(std::memory_order_relaxed) +
            outcomes[2].load(std::memory_order_relaxed);
        EXPECT_EQ(applied, 1);
        EXPECT_TRUE(iaisf::task::is_terminal(
            repository.get_snapshot(id).value().state));
    }
}

TEST(TaskRepositoryTest, LateResultCannotOverwriteTimeout) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());
    ASSERT_TRUE(repository.mark_timed_out(
        id, make_error(ErrorCode::InvalidState, "deadline")));

    auto late = repository.mark_succeeded(id, {{"late", true}});

    ASSERT_TRUE(late);
    EXPECT_EQ(late.value(), TransitionOutcome::AlreadyTerminal);
    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::TimedOut);
    EXPECT_FALSE(snapshot.value().result.has_value());
}

TEST(TaskRepositoryTest, LateFailureCannotOverwriteTimeout) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());
    ASSERT_TRUE(repository.mark_timed_out(id));

    auto late = repository.mark_failed(
        id, make_error(ErrorCode::InternalError, "late failure"));

    ASSERT_TRUE(late);
    EXPECT_EQ(late.value(), TransitionOutcome::AlreadyTerminal);
    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::TimedOut);
    EXPECT_EQ(snapshot.value().error->message, "task timed out");
}

TEST(TaskRepositoryTest, StoresFailedAndTimedOutErrorsWithinConfiguredLimit) {
    const auto limits = TaskLimits::create(4, 32, 64, 64, 4).value();
    TaskRepository repository{limits};
    const TaskId failed_id = create_running(repository);
    const TaskId timeout_id = create_running(repository);
    ASSERT_TRUE(repository.mark_failed(
        failed_id, make_error(ErrorCode::InternalError, "sensitive detail")));
    ASSERT_TRUE(repository.mark_timed_out(
        timeout_id, make_error(ErrorCode::InvalidState, "deadline detail")));

    const auto failed = repository.get_snapshot(failed_id);
    const auto timed_out = repository.get_snapshot(timeout_id);
    ASSERT_TRUE(failed);
    ASSERT_TRUE(timed_out);
    EXPECT_EQ(failed.value().state, TaskState::Failed);
    EXPECT_EQ(timed_out.value().state, TaskState::TimedOut);
    EXPECT_EQ(failed.value().error->message, "####");
    EXPECT_EQ(timed_out.value().error->message, "####");
}

TEST(TaskRepositoryTest, TerminalSuccessRejectsLaterTimeoutWithoutOverwrite) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(repository.mark_succeeded(id, {{"ok", true}}));

    auto timeout = repository.mark_timed_out(id);

    ASSERT_TRUE(timeout);
    EXPECT_EQ(timeout.value(), TransitionOutcome::AlreadyTerminal);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Succeeded);
}

TEST(TaskRepositoryTest, OversizedResultLeavesRunningStateUnchanged) {
    const auto limits = TaskLimits::create(4, 32, 64, 8, 16).value();
    TaskRepository repository{limits};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());

    auto result = repository.mark_succeeded(id, "0123456789");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
    auto snapshot = repository.get_snapshot(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Running);
}

TEST(TaskRepositoryTest, ResultValidationFailureCanStillBecomeFailed) {
    const auto limits = TaskLimits::create(4, 32, 64, 8, 16).value();
    TaskRepository repository{limits};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());
    const std::string invalid_utf8(1, static_cast<char>(0xFF));

    auto invalid_result =
        repository.mark_succeeded(id, nlohmann::json{invalid_utf8});
    ASSERT_FALSE(invalid_result);
    EXPECT_EQ(invalid_result.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Running);

    const auto failed = repository.mark_failed(
        id,
        make_error(ErrorCode::InternalError, "invalid result"));
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value(), TransitionOutcome::Applied);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Failed);
}

TEST(TaskRepositoryTest, RollbackOnlyRemovesQueuedRecords) {
    TaskRepository repository{default_limits()};
    auto queued = repository.create_queued(TaskRequest{"echo", {}});
    ASSERT_TRUE(queued);
    EXPECT_TRUE(repository.rollback_queued(queued.value()));
    EXPECT_EQ(repository.size(), 0U);

    const TaskId running = create_running(repository);
    EXPECT_FALSE(repository.rollback_queued(running));
}

TEST(TaskRepositoryTest, EraseRequiresTerminalState) {
    TaskRepository repository{default_limits()};
    auto queued = repository.create_queued(TaskRequest{"echo", {}});
    ASSERT_TRUE(queued);

    auto erased = repository.erase_terminal(queued.value());

    EXPECT_FALSE(erased);
    EXPECT_EQ(erased.error().code, ErrorCode::InvalidState);
}

TEST(TaskRepositoryTest, MissingIdsReturnNotFound) {
    TaskRepository repository{default_limits()};

    EXPECT_EQ(
        repository.get_snapshot(TaskId{999}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        repository.mark_running(TaskId{999}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        repository.mark_succeeded(TaskId{999}, {}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        repository
            .mark_failed(
                TaskId{999},
                make_error(ErrorCode::InternalError, "missing"))
            .error()
            .code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        repository.mark_timed_out(TaskId{999}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        repository.erase_terminal(TaskId{999}).error().code,
        ErrorCode::NotFound);
}

TEST(TaskRepositoryTest, ConcurrentSnapshotsRemainValidDuringTransition) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(id.valid());
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> valid_snapshots{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&] {
            gate.wait();
            const auto snapshot = repository.get_snapshot(id);
            if (snapshot &&
                (snapshot.value().state == TaskState::Running ||
                 snapshot.value().state == TaskState::Succeeded)) {
                valid_snapshots.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    release.set_value();
    ASSERT_TRUE(repository.mark_succeeded(id, {{"ok", true}}));
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(valid_snapshots.load(std::memory_order_relaxed), 4);
    EXPECT_EQ(repository.get_snapshot(id).value().state, TaskState::Succeeded);
}

TEST(TaskRepositoryTest, ConcurrentSnapshotEraseAndLateTransitionStayConsistent) {
    TaskRepository repository{default_limits()};
    const TaskId id = create_running(repository);
    ASSERT_TRUE(repository.mark_timed_out(id));
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> snapshot_outcome{0};
    std::atomic<int> late_outcome{0};
    std::atomic<bool> erased{false};

    std::thread reader{[&] {
        gate.wait();
        const auto snapshot = repository.get_snapshot(id);
        if (snapshot && snapshot.value().state == TaskState::TimedOut) {
            snapshot_outcome.store(1, std::memory_order_relaxed);
        } else if (!snapshot && snapshot.error().code == ErrorCode::NotFound) {
            snapshot_outcome.store(2, std::memory_order_relaxed);
        }
    }};
    std::thread late_completion{[&] {
        gate.wait();
        const auto completed =
            repository.mark_succeeded(id, {{"late", true}});
        if (completed &&
            completed.value() == TransitionOutcome::AlreadyTerminal) {
            late_outcome.store(1, std::memory_order_relaxed);
        } else if (!completed &&
                   completed.error().code == ErrorCode::NotFound) {
            late_outcome.store(2, std::memory_order_relaxed);
        }
    }};
    std::thread eraser{[&] {
        gate.wait();
        erased.store(
            static_cast<bool>(repository.erase_terminal(id)),
            std::memory_order_relaxed);
    }};

    release.set_value();
    reader.join();
    late_completion.join();
    eraser.join();

    EXPECT_NE(snapshot_outcome.load(std::memory_order_relaxed), 0);
    EXPECT_NE(late_outcome.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(erased.load(std::memory_order_relaxed));
    EXPECT_EQ(repository.get_snapshot(id).error().code, ErrorCode::NotFound);
}

}  // namespace
