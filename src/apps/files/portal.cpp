#include "apps/files/portal.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <gio/gio.h>
#include <memory>
#include <sstream>
#include <unistd.h>

namespace okaru::files {

  namespace {

    constexpr char kSeparator = '\x1f'; // separa partes de --filter
    constexpr const char* kBusName = "org.freedesktop.impl.portal.desktop.okaru";
    constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";

    constexpr const char* kIntrospection = R"(
<node>
  <interface name="org.freedesktop.impl.portal.FileChooser">
    <method name="OpenFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFiles">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.Request">
    <method name="Close"/>
  </interface>
</node>)";

    // Un pedido en curso: el selector hijo y la respuesta pendiente.
    struct Pending {
      GDBusMethodInvocation* invocation = nullptr;
      GPid pid = 0;
      int out = -1;
      std::string data;
      std::string handle;
      guint requestId = 0;
      GDBusConnection* bus = nullptr;
      bool saveFiles = false;
      std::vector<std::string> names; // SaveFiles: nombres a guardar en la carpeta elegida
    };

    GDBusNodeInfo* nodeInfo() {
      static GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
      return info;
    }

    std::string bytesToString(GVariant* value) {
      // "ay" con el NUL final (como las rutas de los portales); sin NUL, se copia el arreglo tal cual.
      if (!g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
        return {};
      }
      gsize size = 0;
      const auto* data = static_cast<const char*>(g_variant_get_fixed_array(value, &size, sizeof(guchar)));
      std::string out(data != nullptr ? data : "", size);
      if (const auto nul = out.find('\0'); nul != std::string::npos) {
        out.resize(nul);
      }
      return out;
    }

    PickFilter parseFilter(GVariant* filter) {
      PickFilter out;
      const char* name = nullptr;
      GVariantIter* rules = nullptr;
      g_variant_get(filter, "(&sa(us))", &name, &rules);
      out.name = name != nullptr ? name : "";
      guint32 kind = 0;
      const char* pattern = nullptr;
      while (g_variant_iter_next(rules, "(u&s)", &kind, &pattern)) {
        (kind == 0 ? out.globs : out.mimes).emplace_back(pattern);
      }
      g_variant_iter_free(rules);
      return out;
    }

    void finish(Pending* pending, guint32 response, GVariant* results) {
      g_dbus_method_invocation_return_value(pending->invocation, g_variant_new("(u@a{sv})", response, results));
      if (pending->requestId != 0) {
        g_dbus_connection_unregister_object(pending->bus, pending->requestId);
      }
      delete pending;
    }

    void onChildExit(GPid pid, gint status, gpointer data) {
      std::unique_ptr<Pending> owned(static_cast<Pending*>(data));
      Pending* pending = owned.release();
      // Lo que imprimió el selector: una URI por línea.
      char buffer[4096];
      ssize_t n = 0;
      while ((n = ::read(pending->out, buffer, sizeof(buffer))) > 0) {
        pending->data.append(buffer, static_cast<std::size_t>(n));
      }
      ::close(pending->out);
      g_spawn_close_pid(pid);
      GVariantBuilder uris;
      g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
      std::istringstream lines(pending->data);
      std::size_t count = 0;
      for (std::string line; std::getline(lines, line);) {
        if (line.empty()) {
          continue;
        }
        if (pending->saveFiles) {
          // SaveFiles: se eligió una carpeta; las URIs son carpeta + cada nombre pedido.
          for (const auto& name : pending->names) {
            GFile* folder = g_file_new_for_uri(line.c_str());
            GFile* child = g_file_get_child(folder, name.c_str());
            gchar* uri = g_file_get_uri(child);
            g_variant_builder_add(&uris, "s", uri);
            g_free(uri);
            g_object_unref(child);
            g_object_unref(folder);
            ++count;
          }
          break;
        }
        g_variant_builder_add(&uris, "s", line.c_str());
        ++count;
      }
      GVariantBuilder results;
      g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
      const bool ok = g_spawn_check_wait_status(status, nullptr) != FALSE && count > 0;
      g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
      finish(pending, ok ? 0 : 1, g_variant_builder_end(&results));
    }

    void onRequestCall(
        GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*,
        GDBusMethodInvocation* invocation, gpointer data
    ) {
      // Close: la app cerró su pedido; el selector se cierra y el pedido termina como cancelado.
      if (g_strcmp0(method, "Close") == 0) {
        ::kill(static_cast<Pending*>(data)->pid, SIGTERM);
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    void onCall(
        GDBusConnection* bus, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* parameters,
        GDBusMethodInvocation* invocation, gpointer data
    ) {
      const auto* self = static_cast<const std::string*>(data);
      const char* handle = nullptr;
      const char* appId = nullptr;
      const char* parent = nullptr;
      const char* title = nullptr;
      GVariant* options = nullptr;
      g_variant_get(parameters, "(&o&s&s&s@a{sv})", &handle, &appId, &parent, &title, &options);

      PickerOptions pick;
      pick.title = title != nullptr ? title : "";
      const bool save = g_strcmp0(method, "SaveFile") == 0;
      const bool saveFiles = g_strcmp0(method, "SaveFiles") == 0;
      pick.mode = save ? PickMode::Save : saveFiles ? PickMode::Folder : PickMode::Open;
      gboolean flag = FALSE;
      if (g_variant_lookup(options, "multiple", "b", &flag)) {
        pick.multiple = flag != FALSE;
      }
      if (g_variant_lookup(options, "directory", "b", &flag) && flag != FALSE) {
        pick.mode = PickMode::Folder;
      }
      const char* text = nullptr;
      if (g_variant_lookup(options, "accept_label", "&s", &text)) {
        pick.acceptLabel = text;
      }
      if (g_variant_lookup(options, "current_name", "&s", &text)) {
        pick.name = text;
      }
      if (GVariant* folder = g_variant_lookup_value(options, "current_folder", G_VARIANT_TYPE_BYTESTRING);
          folder != nullptr) {
        pick.folder = bytesToString(folder);
        g_variant_unref(folder);
      }
      if (GVariant* file = g_variant_lookup_value(options, "current_file", G_VARIANT_TYPE_BYTESTRING);
          file != nullptr) {
        // Guardar sobre un archivo existente: su carpeta y su nombre.
        const std::string path = bytesToString(file);
        gchar* dir = g_path_get_dirname(path.c_str());
        gchar* base = g_path_get_basename(path.c_str());
        pick.folder = dir;
        pick.name = base;
        g_free(dir);
        g_free(base);
        g_variant_unref(file);
      }
      if (GVariant* filters = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
          filters != nullptr) {
        GVariantIter iter;
        g_variant_iter_init(&iter, filters);
        while (GVariant* filter = g_variant_iter_next_value(&iter)) {
          pick.filters.push_back(parseFilter(filter));
          g_variant_unref(filter);
        }
        g_variant_unref(filters);
      }
      if (GVariant* current = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
          current != nullptr) {
        const PickFilter wanted = parseFilter(current);
        for (std::size_t i = 0; i < pick.filters.size(); ++i) {
          if (pick.filters[i].name == wanted.name) {
            pick.currentFilter = i;
          }
        }
        if (pick.filters.empty()) {
          pick.filters.push_back(wanted);
        }
        g_variant_unref(current);
      }
      auto* pending = new Pending{.invocation = invocation, .handle = handle, .bus = bus, .saveFiles = saveFiles};
      if (saveFiles) {
        if (GVariant* files = g_variant_lookup_value(options, "files", G_VARIANT_TYPE("aay")); files != nullptr) {
          GVariantIter iter;
          g_variant_iter_init(&iter, files);
          while (GVariant* name = g_variant_iter_next_value(&iter)) {
            pending->names.push_back(bytesToString(name));
            g_variant_unref(name);
          }
          g_variant_unref(files);
        }
      }
      g_variant_unref(options);

      std::vector<std::string> args{*self};
      const auto pickArgs = pickerArguments(pick);
      args.insert(args.end(), pickArgs.begin(), pickArgs.end());
      std::vector<gchar*> argv;
      argv.reserve(args.size());
      for (auto& arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);
      GError* error = nullptr;
      if (g_spawn_async_with_pipes(
              nullptr, argv.data(), nullptr, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &pending->pid, nullptr,
              &pending->out, nullptr, &error
          )
          == FALSE) {
        g_clear_error(&error);
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("a{sv}"));
        finish(pending, 2, g_variant_builder_end(&empty));
        return;
      }
      static const GDBusInterfaceVTable requestVTable{onRequestCall, nullptr, nullptr, {}};
      pending->requestId = g_dbus_connection_register_object(
          bus, handle, g_dbus_node_info_lookup_interface(nodeInfo(), "org.freedesktop.impl.portal.Request"),
          &requestVTable, pending, nullptr, nullptr
      );
      g_child_watch_add(pending->pid, onChildExit, pending);
    }

  } // namespace

  std::vector<std::string> pickerArguments(const PickerOptions& o) {
    std::vector<std::string> args{
        "--pick",
        o.mode == PickMode::Save         ? "save"
            : o.mode == PickMode::Folder ? "folder"
                                         : "open"
    };
    if (o.multiple) {
      args.emplace_back("--multiple");
    }
    for (const auto& [flag, value] :
         {std::pair{"--title", &o.title}, std::pair{"--folder", &o.folder}, std::pair{"--name", &o.name},
          std::pair{"--accept", &o.acceptLabel}}) {
      if (!value->empty()) {
        args.emplace_back(flag);
        args.push_back(*value);
      }
    }
    for (const auto& filter : o.filters) {
      std::string packed = filter.name;
      for (const auto& g : filter.globs) {
        packed += kSeparator + std::string("g:") + g;
      }
      for (const auto& m : filter.mimes) {
        packed += kSeparator + std::string("m:") + m;
      }
      args.emplace_back("--filter");
      args.push_back(std::move(packed));
    }
    if (o.currentFilter > 0) {
      args.emplace_back("--current-filter");
      args.push_back(std::to_string(o.currentFilter));
    }
    return args;
  }

  PickerOptions parsePickerArguments(const std::vector<std::string>& args) {
    PickerOptions o;
    for (std::size_t i = 0; i < args.size(); ++i) {
      const std::string& a = args[i];
      const bool hasValue = i + 1 < args.size();
      if (a == "--pick" && hasValue) {
        const std::string& m = args[++i];
        o.mode = m == "save" ? PickMode::Save : m == "folder" ? PickMode::Folder : PickMode::Open;
      } else if (a == "--multiple") {
        o.multiple = true;
      } else if (a == "--title" && hasValue) {
        o.title = args[++i];
      } else if (a == "--folder" && hasValue) {
        o.folder = args[++i];
      } else if (a == "--name" && hasValue) {
        o.name = args[++i];
      } else if (a == "--accept" && hasValue) {
        o.acceptLabel = args[++i];
      } else if (a == "--current-filter" && hasValue) {
        o.currentFilter = static_cast<std::size_t>(std::max(0L, std::strtol(args[++i].c_str(), nullptr, 10)));
      } else if (a == "--filter" && hasValue) {
        PickFilter filter;
        std::istringstream parts(args[++i]);
        std::string part;
        bool first = true;
        while (std::getline(parts, part, kSeparator)) {
          if (first) {
            filter.name = part;
            first = false;
          } else if (part.starts_with("g:")) {
            filter.globs.push_back(part.substr(2));
          } else if (part.starts_with("m:")) {
            filter.mimes.push_back(part.substr(2));
          }
        }
        o.filters.push_back(std::move(filter));
      }
    }
    if (o.currentFilter >= o.filters.size()) {
      o.currentFilter = 0;
    }
    return o;
  }

  int runPortalService(const std::string& self) {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    static std::string selfPath;
    selfPath = self;
    static const GDBusInterfaceVTable vtable{onCall, nullptr, nullptr, {}};
    const guint owner = g_bus_own_name(
        G_BUS_TYPE_SESSION, kBusName, G_BUS_NAME_OWNER_FLAGS_REPLACE,
        [](GDBusConnection* bus, const gchar*, gpointer) {
          g_dbus_connection_register_object(
              bus, kObjectPath,
              g_dbus_node_info_lookup_interface(nodeInfo(), "org.freedesktop.impl.portal.FileChooser"), &vtable,
              &selfPath, nullptr, nullptr
          );
        },
        nullptr, [](GDBusConnection*, const gchar*, gpointer data) { g_main_loop_quit(static_cast<GMainLoop*>(data)); },
        loop, nullptr
    );
    g_main_loop_run(loop);
    g_bus_unown_name(owner);
    g_main_loop_unref(loop);
    return 0;
  }

} // namespace okaru::files
