#include "shell/okaru_auth/okaru_vpn_auth.h"

#include "core/log.h"
#include "shell/okaru_auth/okaru_auth_prompt.h"

#include <filesystem>
#include <gio/gio.h>
#include <glib.h>
#include <sodium.h>

namespace okaru::auth {

  namespace {

    constexpr Logger kLog("vpn-auth");
    constexpr auto kPluginDir = "/usr/lib/NetworkManager/VPN";
    constexpr auto kUiGroup = "VPN Plugin UI";

    std::string keyString(GKeyFile* file, const char* group, const char* key) {
      gchar* value = g_key_file_get_string(file, group, key, nullptr);
      std::string out = value != nullptr ? value : "";
      g_free(value);
      return out;
    }

    bool keyBool(GKeyFile* file, const char* group, const char* key) {
      return g_key_file_get_boolean(file, group, key, nullptr) == TRUE;
    }

    void wipe(std::string& secret) {
      if (!secret.empty()) {
        sodium_memzero(secret.data(), secret.size());
      }
      secret.clear();
    }

    // Ejecuta el auth-dialog con `input` por stdin. En modo externo describe y sale enseguida;
    // `timeout` evita que un plugin colgado congele la shell (la llamada es síncrona).
    std::optional<std::string> runAuthDialog(const std::vector<std::string>& args, const std::string& input) {
      std::vector<const gchar*> argv{"timeout", "5"};
      for (const auto& arg : args) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);
      GError* error = nullptr;
      GSubprocess* proc = g_subprocess_newv(
          argv.data(), static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
          &error
      );
      if (proc == nullptr) {
        kLog.warn("auth-dialog no arrancó: {}", error != nullptr ? error->message : "?");
        g_clear_error(&error);
        return std::nullopt;
      }
      gchar* out = nullptr;
      const bool ok = g_subprocess_communicate_utf8(proc, input.c_str(), nullptr, &out, nullptr, &error) == TRUE &&
                      g_subprocess_get_if_exited(proc) && g_subprocess_get_exit_status(proc) == 0;
      if (!ok) {
        kLog.warn("auth-dialog falló: {}", error != nullptr ? error->message : "salida distinta de 0");
      }
      g_clear_error(&error);
      g_object_unref(proc);
      std::optional<std::string> result = ok && out != nullptr ? std::optional<std::string>(out) : std::nullopt;
      g_free(out);
      return result;
    }

  } // namespace

  std::optional<std::string> findExternalAuthDialog(std::string_view serviceType, const std::string& pluginDir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(pluginDir, ec)) {
      if (entry.path().extension() != ".name") {
        continue;
      }
      GKeyFile* file = g_key_file_new();
      std::optional<std::string> dialog;
      if (g_key_file_load_from_file(file, entry.path().c_str(), G_KEY_FILE_NONE, nullptr) == TRUE &&
          keyString(file, "VPN Connection", "service") == serviceType &&
          keyBool(file, "GNOME", "supports-external-ui-mode")) {
        const std::string path = keyString(file, "GNOME", "auth-dialog");
        if (!path.empty()) {
          dialog = path;
        }
      }
      g_key_file_free(file);
      if (dialog.has_value()) {
        return dialog;
      }
    }
    return std::nullopt;
  }

  std::string encodeAuthDialogInput(const std::map<std::string, std::string>& data) {
    std::string out;
    for (const auto& [key, value] : data) {
      out += "DATA_KEY=" + key + "\nDATA_VAL=" + value + "\n\n";
    }
    out += "DONE\n\n";
    return out;
  }

  std::optional<VpnDialogDescription> parseAuthDialogOutput(std::string_view output) {
    GKeyFile* file = g_key_file_new();
    if (g_key_file_load_from_data(file, output.data(), output.size(), G_KEY_FILE_NONE, nullptr) != TRUE ||
        g_key_file_has_group(file, kUiGroup) != TRUE) {
      g_key_file_free(file);
      return std::nullopt;
    }
    VpnDialogDescription desc{
        .title = keyString(file, kUiGroup, "Title"),
        .description = keyString(file, kUiGroup, "Description"),
        .fields = {},
    };
    gsize count = 0;
    gchar** groups = g_key_file_get_groups(file, &count);
    for (gsize i = 0; i < count; ++i) {
      if (std::string_view(groups[i]) == kUiGroup) {
        continue;
      }
      desc.fields.push_back({
          .key = groups[i],
          .label = keyString(file, groups[i], "Label"),
          .value = keyString(file, groups[i], "Value"),
          .shouldAsk = keyBool(file, groups[i], "ShouldAsk"),
      });
    }
    g_strfreev(groups);
    g_key_file_free(file);
    return desc;
  }

  VpnAuthFlow::VpnAuthFlow(NetworkSecretAgent& agent, AuthPromptService& prompts) : m_agent(agent), m_prompts(prompts) {
    m_agent.setVpnRequestCallback(
        [this](const NetworkSecretAgent::VpnRequest& request) { onRequest(request); },
        [this] {
          // NM retiró el pedido (expiró nmcli, se canceló la conexión): cerrar el diálogo.
          const std::string id = m_promptId;
          reset();
          if (!id.empty()) {
            m_prompts.cancel(id);
          }
        }
    );
  }

  VpnAuthFlow::~VpnAuthFlow() {
    m_agent.setVpnRequestCallback(nullptr, nullptr);
    reset();
  }

  void VpnAuthFlow::onRequest(const NetworkSecretAgent::VpnRequest& request) {
    reset();
    const auto dialog = findExternalAuthDialog(request.serviceType, kPluginDir);
    if (!dialog.has_value()) {
      // shortcut: sin modo externo (p. ej. openconnect) no se pide nada; mostrar su propio diálogo si llega a hacer falta.
      kLog.warn("{} no tiene auth-dialog con modo externo", request.serviceType);
      m_agent.cancelSecret();
      return;
    }
    std::vector<std::string> args{
        *dialog, "-u", request.uuid, "-n", request.name, "-s", request.serviceType, "--external-ui-mode", "-i",
    };
    for (const auto& hint : request.hints) {
      args.insert(args.end(), {"-t", hint});
    }
    const auto output = runAuthDialog(args, encodeAuthDialogInput(request.data));
    const auto desc = output.has_value() ? parseAuthDialogOutput(*output) : std::nullopt;
    if (!desc.has_value()) {
      m_agent.cancelSecret();
      return;
    }
    m_title = desc->title.empty() ? request.name : desc->title;
    m_description = desc->description;
    for (const auto& field : desc->fields) {
      if (field.shouldAsk) {
        m_pending.push_back(field);
      } else if (!field.value.empty()) {
        m_answers[field.key] = field.value;
      }
    }
    askNext();
  }

  void VpnAuthFlow::askNext() {
    if (m_pending.empty()) {
      m_agent.submitVpnSecrets(m_answers);
      reset();
      return;
    }
    const VpnField field = m_pending.front();
    m_pending.erase(m_pending.begin());
    m_promptId = m_prompts.open({
        .id = {},
        .requester = {},
        .requesterPid = 0,
        .clientPid = 0,
        .prompt = field.label,
        .message = m_description,
        .title = m_title,
        .attempt = 1,
        .onDone =
            [this, key = field.key](AuthResult result) {
              m_promptId.clear();
              if (result.kind != AuthResultKind::Answered) {
                reset();
                m_agent.cancelSecret();
                return;
              }
              m_answers[key] = std::move(result.secret);
              askNext();
            },
    });
    if (m_promptId.empty()) {
      reset();
      m_agent.cancelSecret();
    }
  }

  void VpnAuthFlow::reset() {
    for (auto& [key, value] : m_answers) {
      wipe(value);
    }
    m_answers.clear();
    m_pending.clear();
    m_promptId.clear();
  }

} // namespace okaru::auth
