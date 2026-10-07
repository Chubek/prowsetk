#ifndef PROWSETK_DRIVER_PANEL_HPP
#define PROWSETK_DRIVER_PANEL_HPP
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "prowsetk/event_ir.hpp"
namespace prowsetk {
inline constexpr std::size_t max_driver_panel_events = 400000;
class DriverPanel {
public:
  virtual ~DriverPanel() = default;
  virtual std::string_view name() const noexcept = 0;
  virtual void set_events(ProwseEventStream events) = 0;
  virtual void set_status(std::string_view status) = 0;
  virtual bool visible() const noexcept = 0;
  virtual void set_visible(bool visible) noexcept = 0;
  virtual std::vector<std::string> actions() const = 0;
  virtual bool invoke(std::string_view action) = 0;
};
class DefaultDriverPanel final : public DriverPanel {
public:
  using Action = std::function<bool(DefaultDriverPanel&)>;
  DefaultDriverPanel();
  std::string_view name() const noexcept override { return "driver"; }
  void set_events(ProwseEventStream events) override;
  void set_status(std::string_view status) override;
  bool visible() const noexcept override;
  void set_visible(bool visible) noexcept override;
  std::vector<std::string> actions() const override;
  bool invoke(std::string_view action) override;
  void add_action(std::string name, Action action);
  ProwseEventStream events() const;
  std::string status() const;
private:
  mutable std::mutex mutex_;
  ProwseEventStream events_;
  std::string status_;
  bool visible_ = true;
  std::unordered_map<std::string, Action> actions_;
};
using DriverPanelFactory = std::function<std::unique_ptr<DriverPanel>()>;
class DriverPanelRegistry {
public:
  static DriverPanelRegistry& global();
  bool register_factory(std::string name, DriverPanelFactory factory);
  bool unregister(std::string_view name);
  std::vector<std::string> names() const;
  std::unique_ptr<DriverPanel> create(std::string_view name) const;
private:
  DriverPanelRegistry();
  mutable std::mutex mutex_;
  std::unordered_map<std::string, DriverPanelFactory> factories_;
};
}
#endif
