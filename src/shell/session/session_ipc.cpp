#include "shell/session/session_ipc.h"

#include "cli/parse.h"
#include "cli/schema_msg.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

  // Same bounds as lockscreen.grace_period_seconds in the config schema.
  constexpr int kMinGraceSeconds = 0;
  constexpr int kMaxGraceSeconds = 60;

  [[nodiscard]] SessionPanelActionConfig builtinAction(std::string_view action) {
    return SessionPanelActionConfig{.action = std::string(action)};
  }

  [[nodiscard]] std::string unknownSessionActionError(std::string_view action) {
    return "error: unknown session action \""
        + std::string(action)
        + "\" (try: lock, suspend, lock-and-suspend, logout, reboot, reboot-uefi, shutdown)\n";
  }

} // namespace

void registerSessionIpc(IpcService& ipc, SessionActionRunner& runner, LockScreen& lockScreen, ConfigService& config) {
  const auto dispatch = [&runner, &lockScreen, &config](const std::string& args) -> std::string {
    auto tokens = noctalia::ipc::splitWords(args);
    std::vector<char*> pointers;
    pointers.reserve(tokens.size());
    for (std::string& token : tokens) {
      pointers.push_back(token.data());
    }
    // ParsedArgs holds views into `tokens`, which therefore outlives it.
    const auto parsed = noctalia::cli::parseArgs(noctalia::cli::msg::session, pointers);
    if (!parsed) {
      return parsed.error() + "\n";
    }
    if (parsed->positionals.empty()) {
      return "error: session requires <lock|suspend|lock-and-suspend|logout|reboot|reboot-uefi|shutdown>\n";
    }

    const std::string ipcAction(parsed->positionals[0]);
    const auto action = session_action::canonicalActionName(ipcAction);
    if (!action.has_value()) {
      return unknownSessionActionError(ipcAction);
    }

    std::optional<int> graceSeconds;
    if (parsed->has("--grace")) {
      if (*action != "lock") {
        return "error: --grace only applies to lock\n";
      }
      const std::string_view value = parsed->value("--grace");
      int seconds = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
      if (ec != std::errc{} || end != value.data() + value.size() || seconds < kMinGraceSeconds
          || seconds > kMaxGraceSeconds) {
        return "error: --grace expects whole seconds from 0 to 60\n";
      }
      graceSeconds = seconds;
    }

    if (*action == "lock") {
      if (!config.isLockScreenEnabled()) {
        return "error: lock screen disabled\n";
      }
      if (lockScreen.lock(graceSeconds)) {
        return "ok\n";
      }
      return "error: lock screen unavailable\n";
    }
    if (*action == "lock_and_suspend") {
      if (!config.isLockScreenEnabled()) {
        runner.invoke(builtinAction("suspend"));
        return "ok\n";
      }
      runner.invoke(builtinAction(*action));
      return "ok\n";
    }

    runner.invoke(builtinAction(*action));
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::session, dispatch);
}
