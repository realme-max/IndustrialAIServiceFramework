#pragma once

#include <cstddef>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/timer.hpp"
#include "iaisf/service/service_options.hpp"

namespace iaisf::service {

/** Fully validated Linux runtime values compiled from portable AppConfig. */
class RuntimeOptions final {
  public:
    RuntimeOptions(std::size_t reactor_max_events,
                   std::size_t pending_callback_capacity,
                   net::TimerQueueOptions timer_options,
                   net::tcp::Ipv4Endpoint bind_endpoint,
                   ServiceOptions service_options) noexcept;

    [[nodiscard]] std::size_t reactor_max_events() const noexcept;
    [[nodiscard]] std::size_t pending_callback_capacity() const noexcept;
    [[nodiscard]] net::TimerQueueOptions timer_options() const noexcept;
    [[nodiscard]] const net::tcp::Ipv4Endpoint &bind_endpoint() const noexcept;
    [[nodiscard]] const ServiceOptions &service_options() const noexcept;

  private:
    std::size_t reactor_max_events_;
    std::size_t pending_callback_capacity_;
    net::TimerQueueOptions timer_options_;
    net::tcp::Ipv4Endpoint bind_endpoint_;
    ServiceOptions service_options_;
};

[[nodiscard]] Result<RuntimeOptions>
make_runtime_options(const AppConfig &config);

} // namespace iaisf::service
