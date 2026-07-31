#pragma once

#include <atomic>
#include <memory>

#include "iaisf/api/task_api_limits.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/plugin/plugin_manager.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/task_manager.hpp"

namespace iaisf::api {

/**
 * Fast EventLoop-side HTTP facade over TaskManager.
 *
 * The facade owns no EventLoop, worker, repository or plugin. Route handlers
 * capture a weak facade token. The referenced managers must outlive all route
 * dispatch and this object.
 */
class TaskHttpApi final : public std::enable_shared_from_this<TaskHttpApi> {
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

public:
    using Ptr = std::shared_ptr<TaskHttpApi>;

    [[nodiscard]] static Result<Ptr> create(
        task::TaskManager& task_manager,
        const plugin::PluginManager& plugin_manager,
        task::TaskLimits task_limits,
        http::HttpLimits http_limits,
        TaskApiLimits api_limits = TaskApiLimits::defaults());

    TaskHttpApi(const TaskHttpApi&) = delete;
    TaskHttpApi& operator=(const TaskHttpApi&) = delete;
    TaskHttpApi(TaskHttpApi&&) = delete;
    TaskHttpApi& operator=(TaskHttpApi&&) = delete;

    TaskHttpApi(
        ConstructionKey,
        task::TaskManager& task_manager,
        const plugin::PluginManager& plugin_manager,
        task::TaskLimits task_limits,
        http::HttpLimits http_limits,
        TaskApiLimits api_limits) noexcept;

    [[nodiscard]] Result<void> register_routes(http::HttpRouter& router);
    void stop_admission() noexcept;
    [[nodiscard]] bool accepting_submissions() const noexcept;

private:
    [[nodiscard]] Result<http::HttpResponse> submit_task(
        const http::HttpRequest& request);
    [[nodiscard]] Result<http::HttpResponse> get_task(
        const http::HttpRequest& request,
        const std::string& task_id_text);
    [[nodiscard]] Result<http::HttpResponse> routing_error(
        http::HttpStatus status,
        const http::HttpRequest& request);

    task::TaskManager& task_manager_;
    const plugin::PluginManager& plugin_manager_;
    task::TaskLimits task_limits_;
    http::HttpLimits http_limits_;
    TaskApiLimits api_limits_;
    std::atomic<bool> accepting_submissions_{true};
};

}  // namespace iaisf::api
