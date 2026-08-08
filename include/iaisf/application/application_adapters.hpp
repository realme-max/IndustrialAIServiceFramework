#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "iaisf/application/application_job.hpp"
#include "iaisf/application/application_output_artifacts.hpp"
#include "iaisf/application/application_pointcloud.hpp"
#include "iaisf/application/application_process.hpp"

namespace iaisf::application {

class LocalArtifactCatalog;

struct Ptv2AdapterOptions final {
    std::filesystem::path executable;
    std::filesystem::path engine;
    std::filesystem::path plugin;
    std::filesystem::path working_directory;
    std::filesystem::path scratch_root;
    std::filesystem::path output_root;
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    std::size_t max_stdout_bytes{4U * 1024U * 1024U};
    std::size_t max_stderr_bytes{4U * 1024U * 1024U};
    std::size_t max_result_json_bytes{256U * 1024U};
};

class Ptv2WeldInspectionAdapter final {
public:
    [[nodiscard]] static Result<std::unique_ptr<Ptv2WeldInspectionAdapter>>
    create(Ptv2AdapterOptions options,
           const LocalArtifactResolver& resolver,
           IProcessRunner& runner,
           std::shared_ptr<LocalArtifactCatalog> catalog = nullptr);

    [[nodiscard]] Result<ApplicationExecutionResult> execute(
        const ApplicationJobSnapshot& snapshot);

private:
    Ptv2WeldInspectionAdapter(
        Ptv2AdapterOptions options,
        const LocalArtifactResolver& resolver,
        IProcessRunner& runner,
        std::unique_ptr<PointCloudTxtMaterializer> materializer,
        std::unique_ptr<LocalOutputArtifactRegistrar> registrar);

    Ptv2AdapterOptions options_;
    const LocalArtifactResolver& resolver_;
    IProcessRunner& runner_;
    std::unique_ptr<PointCloudTxtMaterializer> materializer_;
    std::unique_ptr<LocalOutputArtifactRegistrar> registrar_;
};

struct WeldAgentAdapterOptions final {
    std::filesystem::path python_executable;
    std::filesystem::path orchestrator;
    std::filesystem::path tool_config;
    std::filesystem::path project_root;
    std::filesystem::path scratch_root;
    std::filesystem::path output_root;
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    std::size_t max_stdout_bytes{4U * 1024U * 1024U};
    std::size_t max_stderr_bytes{4U * 1024U * 1024U};
    std::size_t max_json_bytes{256U * 1024U};
};

class WeldAgentWeldingGuidanceAdapter final {
public:
    [[nodiscard]] static Result<std::unique_ptr<WeldAgentWeldingGuidanceAdapter>>
    create(WeldAgentAdapterOptions options,
           const LocalArtifactResolver& resolver,
           IProcessRunner& runner,
           std::shared_ptr<LocalArtifactCatalog> catalog = nullptr);

    [[nodiscard]] Result<ApplicationExecutionResult> execute(
        const ApplicationJobSnapshot& snapshot);

private:
    WeldAgentWeldingGuidanceAdapter(
        WeldAgentAdapterOptions options,
        const LocalArtifactResolver& resolver,
        IProcessRunner& runner,
        std::unique_ptr<PointCloudTxtMaterializer> materializer,
        std::unique_ptr<LocalOutputArtifactRegistrar> registrar);

    WeldAgentAdapterOptions options_;
    const LocalArtifactResolver& resolver_;
    IProcessRunner& runner_;
    std::unique_ptr<PointCloudTxtMaterializer> materializer_;
    std::unique_ptr<LocalOutputArtifactRegistrar> registrar_;
};

}  // namespace iaisf::application
