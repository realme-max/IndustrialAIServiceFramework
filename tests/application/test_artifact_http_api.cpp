#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "iaisf/application/artifact_http_api.hpp"
#include "iaisf/application/application_output_artifacts.hpp"

namespace iaisf::application {
namespace {

class ArtifactHttpApiTest : public ::testing::Test {
protected:
    static std::string header_value(
        const http::HttpResponse& response, const std::string& name) {
        for (const auto& header : response.headers()) {
            if (header.name == name) return header.value;
        }
        return {};
    }

    void SetUp() override {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root_ = std::filesystem::temp_directory_path() / ("iaisf-artifact-" + suffix);
        artifact_root_ = root_ / "artifacts";
        output_root_ = root_ / "outputs";
        std::filesystem::create_directories(artifact_root_);
        std::filesystem::create_directories(output_root_);
        auto catalog = LocalArtifactCatalog::make(artifact_root_, output_root_);
        ASSERT_TRUE(catalog);
        catalog_ = std::move(catalog).value();
        auto api = ArtifactHttpApi::create(
            artifact_root_, catalog_, http::HttpLimits::defaults());
        ASSERT_TRUE(api);
        api_ = std::move(api).value();
        ASSERT_TRUE(api_->register_routes(router_));
        ASSERT_TRUE(router_.freeze());
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    http::HttpResponse dispatch(
        std::string method, std::string target, std::string body,
        std::string content_type = "text/plain") {
        return dispatch_on(router_, std::move(method), std::move(target),
                           std::move(body), std::move(content_type));
    }

    http::HttpResponse dispatch_on(
        http::HttpRouter& router, std::string method, std::string target,
        std::string body, std::string content_type = "text/plain") {
        http::HttpRequest::Headers headers;
        headers.push_back({"content-type", std::move(content_type)});
        auto request = http::HttpRequest::create(
            std::move(method), std::move(target), std::move(headers),
            std::move(body), true);
        EXPECT_TRUE(request);
        auto response = router.dispatch(request.value());
        EXPECT_TRUE(response);
        return std::move(response).value();
    }

    std::filesystem::path root_;
    std::filesystem::path artifact_root_;
    std::filesystem::path output_root_;
    std::shared_ptr<LocalArtifactCatalog> catalog_;
    ArtifactHttpApi::Ptr api_;
    http::HttpRouter router_;
};

TEST_F(ArtifactHttpApiTest, UploadsThreeAndFourColumnTextAndDownloadsCanonicalBytes) {
    const auto upload = dispatch(
        "POST", "/api/artifacts/v1/pointclouds",
        " \t\r\n1 2 3\r\n\t4\t5\t6\textra\r\n");
    EXPECT_EQ(upload.status(), http::HttpStatus::Created);
    const auto body = nlohmann::json::parse(upload.body());
    ASSERT_EQ(body.at("artifact").at("point_count"), 2U);
    const auto id = body.at("artifact").at("artifact_id").get<std::string>();
    EXPECT_EQ(body.at("download_url"), "/api/artifacts/v1/files/" + id);

    const auto download = dispatch(
        "GET", "/api/artifacts/v1/files/" + id, "", "text/plain");
    EXPECT_EQ(download.status(), http::HttpStatus::Ok);
    EXPECT_EQ(download.body().size(), 24U);
    EXPECT_EQ(header_value(download, "Content-Type"),
              "application/vnd.iaisf.pointcloud.xyz-f32le");
    EXPECT_EQ(header_value(download, "X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(header_value(download, "Cache-Control"), "no-store");
    EXPECT_NE(header_value(download, "Content-Disposition").find(id),
              std::string::npos);
}

TEST_F(ArtifactHttpApiTest, RepeatedContentIsIdempotentAndInvalidInputIsRejected) {
    const auto first = dispatch(
        "POST", "/api/artifacts/v1/pointclouds", "0 0 0\n");
    ASSERT_EQ(first.status(), http::HttpStatus::Created);
    const auto second = dispatch(
        "POST", "/api/artifacts/v1/pointclouds", "0 0 0\n");
    EXPECT_EQ(second.status(), http::HttpStatus::Ok);
    EXPECT_EQ(first.body(), second.body());

    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "").status(),
              http::HttpStatus::BadRequest);
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "1 2\n").status(),
              http::HttpStatus::BadRequest);
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "nan 2 3\n").status(),
              http::HttpStatus::UnprocessableContent);
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "1 2 3\n",
                        "application/json").status(),
              http::HttpStatus::UnsupportedMediaType);
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds",
                       "1e100 2 3\n").status(),
              http::HttpStatus::UnprocessableContent);
}

TEST_F(ArtifactHttpApiTest, InvalidAndUnknownIdsFailClosed) {
    EXPECT_EQ(dispatch("GET", "/api/artifacts/v1/files/../secret", "").status(),
              http::HttpStatus::NotFound);
    EXPECT_EQ(dispatch("GET", "/api/artifacts/v1/files/%2e%2e", "").status(),
              http::HttpStatus::BadRequest);
    EXPECT_EQ(dispatch("GET", "/api/artifacts/v1/files/not_found", "").status(),
              http::HttpStatus::NotFound);
}

TEST_F(ArtifactHttpApiTest, RegisteredOutputArtifactsAreDownloadable) {
    const auto output = output_root_ / "weld_result.json";
    {
        std::ofstream file(output, std::ios::binary);
        file << "{\"mock\":true}";
    }
    auto registrar = LocalOutputArtifactRegistrar::make(
        output_root_, catalog_);
    ASSERT_TRUE(registrar);
    auto artifact = registrar.value()->register_file(
        output,
        OutputArtifactSpec{"weld-result", "result", "application/json",
                           std::nullopt, std::nullopt, std::nullopt});
    ASSERT_TRUE(artifact);

    const auto response = dispatch(
        "GET", "/api/artifacts/v1/files/weld-result", "");
    EXPECT_EQ(response.status(), http::HttpStatus::Ok);
    EXPECT_EQ(response.body(), "{\"mock\":true}");
    EXPECT_EQ(header_value(response, "Content-Type"), "application/json");
    EXPECT_NE(header_value(response, "Content-Disposition").find(".json"),
              std::string::npos);
}

TEST_F(ArtifactHttpApiTest, DownloadFailsClosedWhenTheFileChanges) {
    const auto upload = dispatch(
        "POST", "/api/artifacts/v1/pointclouds", "1 2 3\n");
    ASSERT_EQ(upload.status(), http::HttpStatus::Created);
    const auto id = nlohmann::json::parse(upload.body())
                        .at("artifact").at("artifact_id").get<std::string>();
    std::ofstream replacement(
        artifact_root_ / "inputs" / id / "pointcloud.xyzf32le",
        std::ios::binary | std::ios::trunc);
    replacement << "changed";
    replacement.close();
    EXPECT_EQ(dispatch(
                  "GET", "/api/artifacts/v1/files/" + id, "").status(),
              http::HttpStatus::ServiceUnavailable);
}

TEST_F(ArtifactHttpApiTest, ExistingManifestMustMatchAndCanBeRepaired) {
    const auto first = dispatch(
        "POST", "/api/artifacts/v1/pointclouds", "1 2 3\n");
    ASSERT_EQ(first.status(), http::HttpStatus::Created);
    const auto id = nlohmann::json::parse(first.body())
                        .at("artifact").at("artifact_id").get<std::string>();
    const auto directory = artifact_root_ / "inputs" / id;
    const auto manifest = directory / "artifact.json";
    std::error_code error;
    std::filesystem::remove(manifest, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "1 2 3\n").status(),
              http::HttpStatus::Ok);
    {
        std::ofstream bad(manifest, std::ios::binary | std::ios::trunc);
        bad << "{\"artifact_id\":\"wrong\"}";
    }
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "1 2 3\n").status(),
              http::HttpStatus::InternalServerError);
    {
        std::ofstream duplicate(manifest, std::ios::binary | std::ios::trunc);
        duplicate << "{\"artifact_id\":\"" << id
                  << "\",\"artifact_id\":\"" << id
                  << "\",\"sha256\":\"bad\"}";
    }
    EXPECT_EQ(dispatch("POST", "/api/artifacts/v1/pointclouds", "1 2 3\n").status(),
              http::HttpStatus::InternalServerError);
}

TEST_F(ArtifactHttpApiTest, ConcurrentIdenticalUploadsCommitOnce) {
    constexpr int kWorkers = 6;
    std::vector<std::thread> workers;
    std::vector<http::HttpStatus> statuses;
    std::mutex statuses_mutex;
    for (int index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&, index] {
            (void)index;
            auto response = dispatch(
                "POST", "/api/artifacts/v1/pointclouds", "7 8 9\n");
            std::lock_guard<std::mutex> lock(statuses_mutex);
            statuses.push_back(response.status());
        });
    }
    for (auto& worker : workers) worker.join();
    ASSERT_EQ(statuses.size(), static_cast<std::size_t>(kWorkers));
    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), http::HttpStatus::Created), 1);
    EXPECT_EQ(std::count(statuses.begin(), statuses.end(), http::HttpStatus::Ok), kWorkers - 1);
}

TEST_F(ArtifactHttpApiTest, CatalogRejectsInvalidIdAndConflictingMetadata) {
    auto output = output_root_ / "catalog.bin";
    {
        std::ofstream file(output, std::ios::binary);
        file << "data";
    }
    ArtifactRef artifact{"-invalid", std::string(64, 'a'), 4U, "result",
                         "application/octet-stream", std::nullopt, std::nullopt,
                         std::nullopt};
    EXPECT_FALSE(catalog_->register_artifact(artifact, output, "artifact-1.bin"));
    artifact.artifact_id = "artifact-1";
    artifact.sha256 = std::string(64, 'b');
    auto first = catalog_->register_artifact(artifact, output, "artifact-1.bin");
    ASSERT_TRUE(first);
    auto conflicting = artifact;
    conflicting.kind = "other";
    EXPECT_EQ(catalog_->register_artifact(conflicting, output, "artifact-1.bin").error().code,
              ErrorCode::InvalidState);
    EXPECT_EQ(catalog_->register_artifact(artifact, output, "different.bin").error().code,
              ErrorCode::InvalidState);
    const auto second_path = output_root_ / "catalog-second.bin";
    {
        std::ofstream file(second_path, std::ios::binary);
        file << "data";
    }
    EXPECT_EQ(catalog_->register_artifact(artifact, second_path, "artifact-1.bin").error().code,
              ErrorCode::InvalidState);
    EXPECT_EQ(catalog_->find("-invalid").error().code, ErrorCode::InvalidArgument);
    const auto outside = root_ / "outside.bin";
    {
        std::ofstream file(outside, std::ios::binary);
        file << "data";
    }
    EXPECT_EQ(catalog_->register_artifact(artifact, outside, "outside.bin").error().code,
              ErrorCode::InvalidArgument);
}

TEST_F(ArtifactHttpApiTest, ConfiguredBodyAndResponseLimitsAreEnforced) {
    std::string cloud;
    for (int index = 0; index < 20; ++index) {
        cloud += std::to_string(index) + " 1 2\n";
    }
    const auto uploaded = dispatch(
        "POST", "/api/artifacts/v1/pointclouds", cloud);
    ASSERT_EQ(uploaded.status(), http::HttpStatus::Created);
    const auto artifact_id = nlohmann::json::parse(uploaded.body())
                                 .at("artifact").at("artifact_id")
                                 .get<std::string>();

    auto limits = http::HttpLimits::create(
        16 * 1024, 32, 8 * 1024, 8 * 1024, 32 * 1024, 100,
        1024, 128, 256, 16);
    ASSERT_TRUE(limits);
    http::HttpRouter router;
    auto api = ArtifactHttpApi::create(artifact_root_, catalog_, limits.value());
    ASSERT_TRUE(api);
    ASSERT_TRUE(api.value()->register_routes(router));
    ASSERT_TRUE(router.freeze());
    const auto oversized = dispatch_on(
        router, "GET", "/api/artifacts/v1/files/" + artifact_id, "");
    EXPECT_EQ(oversized.status(), http::HttpStatus::PayloadTooLarge);
    EXPECT_LE(oversized.body().size(), limits.value().max_response_body_bytes());
    const auto oversized_error = nlohmann::json::parse(oversized.body());
    EXPECT_EQ(oversized_error.at("error").at("code"), "artifact_unavailable");

    auto request_limited = http::HttpLimits::create(
        16 * 1024, 32, 8 * 1024, 8 * 1024, 32 * 1024, 100,
        4, 1024, 256, 16);
    ASSERT_TRUE(request_limited);
    http::HttpRouter request_router;
    auto request_api = ArtifactHttpApi::create(
        artifact_root_, catalog_, request_limited.value());
    ASSERT_TRUE(request_api);
    ASSERT_TRUE(request_api.value()->register_routes(request_router));
    ASSERT_TRUE(request_router.freeze());
    EXPECT_EQ(dispatch_on(request_router, "POST",
                           "/api/artifacts/v1/pointclouds", "1 2 3\n").status(),
              http::HttpStatus::PayloadTooLarge);
}

}  // namespace
}  // namespace iaisf::application
