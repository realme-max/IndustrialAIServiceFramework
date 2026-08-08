#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "iaisf/application/local_artifact_catalog.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::application {

/** HTTP upload/download facade for the process-local artifact catalog. */
class ArtifactHttpApi final
    : public std::enable_shared_from_this<ArtifactHttpApi> {
public:
    using Ptr = std::shared_ptr<ArtifactHttpApi>;

    [[nodiscard]] static Result<Ptr> create(
        std::filesystem::path artifact_root,
        std::shared_ptr<LocalArtifactCatalog> catalog,
        http::HttpLimits limits);

    [[nodiscard]] Result<void> register_routes(http::HttpRouter& router);

private:
    ArtifactHttpApi(
        std::filesystem::path artifact_root,
        std::shared_ptr<LocalArtifactCatalog> catalog,
        http::HttpLimits limits) noexcept;

    [[nodiscard]] Result<http::HttpResponse> upload(
        const http::HttpRequest& request);
    [[nodiscard]] Result<http::HttpResponse> download(
        const http::HttpRequest& request,
        const std::string& artifact_id);

    std::filesystem::path artifact_root_;
    std::shared_ptr<LocalArtifactCatalog> catalog_;
    http::HttpLimits limits_;
    mutable std::mutex upload_mutex_;
};

}  // namespace iaisf::application
