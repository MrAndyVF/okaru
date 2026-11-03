#include "shell/settings/okaru_settings_binds.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_binds.h"
#include "compositors/hyprland/okaru_hypr_rules.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace settings {
  namespace {

    using okaru::hypr::Bind;
    using okaru::hypr::BindAction;
    using okaru::hypr::BindCategory;
    using okaru::hypr::BindFile;
    using Json = nlohmann::ordered_json;

    constexpr BindCategory kCategories[] = {
        BindCategory::System, BindCategory::Media, BindCategory::Input, BindCategory::User, BindCategory::Other,
    };
    constexpr std::string_view kModifiers[] = {"SUPER", "SHIFT", "CTRL", "ALT"};

    // Acciones de la shell más usadas (comandos de `okaru msg`).
    const std::vector<std::string>& shellActions() {
      static const std::vector<std::string> kActions{
          "panel-toggle launcher",
          "panel-toggle clipboard",
          "panel-toggle control-center",
          "panel-toggle session",
          "panel-toggle wallpaper",
          "settings-toggle",
          "window-switcher hold",
          "session lock",
          "caffeine-toggle",
          "nightlight-force-toggle",
          "notification-dnd-toggle",
          "bar-toggle",
          "dock-toggle",
          "screenshot-region",
          "screenshot-window",
          "screenshot-fullscreen",
          "volume-up 5",
          "volume-down 5",
          "volume-mute",
          "mic-mute",
          "media toggle",
          "media next",
          "media previous",
          "brightness-up",
          "brightness-down",
      };
      return kActions;
    }

    std::string_view categoryId(BindCategory c) {
      switch (c) {
      case BindCategory::System:
        return "system";
      case BindCategory::Media:
        return "media";
      case BindCategory::Input:
        return "input";
      case BindCategory::User:
        return "user";
      case BindCategory::Other:
        return "other";
      }
      return "other";
    }

    struct Draft {
      std::vector<std::string> mods;
      std::string key;
      BindAction::Kind kind = BindAction::Kind::Shell;
      std::string text; // comando de la shell, comando del sistema o expresión Lua
      bool locked = false;
      bool repeating = false;
      std::string description;
      Json otherOptions = Json::object(); // opciones sin control propio (mouse, release...), se conservan
      std::string sectionId;              // subcategoría donde se crea (solo atajos nuevos)
    };

    struct BindsState {
      compositors::hyprland::HyprlandRuntime runtime;
      std::function<void()> rebuild;
      BindCategory category = BindCategory::System;
      std::string error;
      // Editor: índice en la lista leída (-1 = nuevo, -2 = ninguno).
      int editing = -2;
      Draft draft;
      int confirmDelete = -1;
    };

    BindsState& state() {
      static BindsState s;
      return s;
    }

    void rebuild() {
      if (state().rebuild) {
        state().rebuild();
      }
    }

    std::string tr(std::string_view key) { return i18n::tr("settings.okaru.binds." + std::string(key)); }

    // Nombre de una subcategoría en el idioma de la interfaz; si el encabezado no se conoce, tal cual.
    std::string sectionLabel(std::string_view header) {
      const std::string id = okaru::hypr::sectionIdOf(header);
      return id.empty() ? std::string(header) : tr("sections." + id);
    }

    std::vector<std::string> sectionsFor(BindCategory category) {
      std::vector<std::string> ids;
      for (const auto& section : okaru::hypr::bindSections()) {
        if (section.category == category) {
          ids.emplace_back(section.id);
        }
      }
      return ids;
    }

    Draft draftFrom(const Bind& bind) {
      Draft d;
      std::string current;
      std::vector<std::string> parts;
      for (const char c : bind.keys) {
        if (c == '+') {
          parts.push_back(current);
          current.clear();
        } else if (c != ' ') {
          current += c;
        }
      }
      parts.push_back(current);
      d.key = parts.back();
      parts.pop_back();
      for (auto& p : parts) {
        std::ranges::transform(p, p.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        d.mods.push_back(p);
      }
      const auto action = okaru::hypr::actionOf(bind);
      d.kind = action.kind;
      d.text = action.text;
      for (const auto& [key, value] : bind.options.items()) {
        if (key == "locked") {
          d.locked = value.is_boolean() && value.get<bool>();
        } else if (key == "repeating") {
          d.repeating = value.is_boolean() && value.get<bool>();
        } else if (key == "description") {
          d.description = value.is_string() ? value.get<std::string>() : std::string{};
        } else {
          d.otherOptions[key] = value;
        }
      }
      return d;
    }

    std::string keysOf(const Draft& d) {
      std::string out;
      for (const auto mod : kModifiers) {
        if (std::ranges::contains(d.mods, std::string(mod))) {
          out += std::string(mod) + " + ";
        }
      }
      return out + d.key;
    }

    // Prefijo de la variable `ipc` de binds.lua ("noctalia msg " u "okaru msg ").
    std::string ipcPrefix() {
      static const std::regex kIpc(R"re(local\s+ipc\s*=\s*"([^"]*)")re");
      const std::string text = okaru::hypr::readBindsFile(BindFile::Binds);
      std::smatch m;
      return std::regex_search(text, m, kIpc) ? m[1].str() : std::string("okaru msg ");
    }

    std::string callOf(const Draft& d, BindFile target) {
      std::string dispatcher;
      switch (d.kind) {
      case BindAction::Kind::Shell:
        // user-binds.lua no tiene la variable `ipc`: ahí va el comando completo.
        dispatcher = target == BindFile::Binds ? okaru::hypr::shellDispatcher(d.text)
                                               : okaru::hypr::commandDispatcher(ipcPrefix() + d.text);
        break;
      case BindAction::Kind::Command:
        dispatcher = okaru::hypr::commandDispatcher(d.text);
        break;
      case BindAction::Kind::Other:
        dispatcher = d.text;
        break;
      }
      Json options = d.otherOptions;
      if (d.locked) {
        options["locked"] = true;
      }
      if (d.repeating) {
        options["repeating"] = true;
      }
      if (!d.description.empty()) {
        options["description"] = d.description;
      }
      return okaru::hypr::serializeBind(keysOf(d), dispatcher, options);
    }

    // Quita la línea completa de un atajo (con su salto de línea).
    std::string removeLine(const std::string& text, const Bind& bind) {
      std::size_t end = bind.end;
      if (end < text.size() && text[end] == '\n') {
        ++end;
      }
      std::string out = text;
      return out.erase(bind.start, end - bind.start);
    }

    std::string appendLine(std::string text, const std::string& line) {
      if (!text.empty() && !text.ends_with('\n')) {
        text += '\n';
      }
      return text + line + "\n";
    }

    void commit(const std::vector<std::string>& before, const std::vector<std::string>& after) {
      auto& s = state();
      if (auto err = okaru::hypr::writeBindFiles(s.runtime, before, after)) {
        s.error = *err;
      } else {
        s.error.clear();
        s.editing = -2;
        s.confirmDelete = -1;
      }
      rebuild();
    }

    void save(const std::vector<Bind>& binds) {
      auto& s = state();
      if (s.draft.key.empty() || s.draft.text.empty()) {
        s.error = tr("need-key-action");
        rebuild();
        return;
      }
      const std::vector<std::string> before{
          okaru::hypr::readBindsFile(BindFile::Binds), okaru::hypr::readBindsFile(BindFile::UserBinds)
      };
      std::vector<std::string> after = before;
      if (s.editing >= 0 && s.editing < static_cast<int>(binds.size())) {
        const auto& bind = binds[static_cast<std::size_t>(s.editing)];
        auto& text = after[bind.file == BindFile::Binds ? 0 : 1];
        text.replace(bind.start, bind.end - bind.start, callOf(s.draft, bind.file));
      } else {
        // Los atajos de apps del usuario van a user-binds.lua; el resto, a binds.lua.
        const BindFile target = s.category == BindCategory::User ? BindFile::UserBinds : BindFile::Binds;
        auto& text = after[target == BindFile::Binds ? 0 : 1];
        // Al final de su subcategoría; si la sección no existe, se crea con su encabezado.
        text = s.draft.sectionId.empty()
            ? appendLine(text, callOf(s.draft, target))
            : okaru::hypr::insertInSection(
                  text, s.draft.sectionId, tr("sections." + s.draft.sectionId), callOf(s.draft, target)
              );
      }
      commit(before, after);
    }

    void removeAt(const std::vector<Bind>& binds, int index) {
      const auto& bind = binds[static_cast<std::size_t>(index)];
      const std::vector<std::string> before{
          okaru::hypr::readBindsFile(BindFile::Binds), okaru::hypr::readBindsFile(BindFile::UserBinds)
      };
      std::vector<std::string> after = before;
      auto& text = after[bind.file == BindFile::Binds ? 0 : 1];
      text = removeLine(text, bind);
      commit(before, after);
    }

    // Lleva atajos de binds.lua a user-binds.lua (de atrás hacia adelante para no mover los rangos).
    void moveToUserBinds(const std::vector<Bind>& binds, std::vector<int> indices) {
      const std::vector<std::string> before{
          okaru::hypr::readBindsFile(BindFile::Binds), okaru::hypr::readBindsFile(BindFile::UserBinds)
      };
      std::vector<std::string> after = before;
      std::ranges::sort(indices, std::greater{});
      std::vector<std::string> lines;
      for (const int i : indices) {
        const auto& bind = binds[static_cast<std::size_t>(i)];
        lines.insert(lines.begin(), before[0].substr(bind.start, bind.end - bind.start));
        after[0] = removeLine(after[0], bind);
      }
      for (const auto& line : lines) {
        after[1] = okaru::hypr::insertInSection(after[1], "apps", tr("sections.apps"), line);
      }
      commit(before, after);
    }

    std::unique_ptr<Button>
    button(float scale, std::string text, std::string glyph, ButtonVariant variant, std::function<void()> onClick) {
      return ui::button({
          .text = std::move(text),
          .glyph = glyph.empty() ? std::nullopt : std::optional<std::string>(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = glyph.empty() ? std::nullopt : std::optional<float>(Style::fontSizeBody * scale),
          .variant = variant,
          .minHeight = Style::controlHeightSm * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Node> textInput(
        float scale, std::string value, float width, std::string placeholder,
        std::function<void(const std::string&)> onChange
    ) {
      Input* raw = nullptr;
      auto input = ui::input({
          .out = &raw,
          .value = std::move(value),
          .placeholder = std::move(placeholder),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .width = width * scale,
      });
      raw->setOnChange(std::move(onChange));
      return input;
    }

    std::unique_ptr<Flex> labeled(float scale, const std::string& label, std::unique_ptr<Node> control) {
      auto column = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          makeLabel(label, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      );
      column->addChild(std::move(control));
      return column;
    }

    void addEditor(Flex& parent, float scale, const std::vector<Bind>& binds) {
      auto& s = state();
      auto editor = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
          .configure = [scale](Flex& box) {
            box.setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
            box.setCardStyle(scale, kCardFillOpacity);
          },
      });

      if (s.editing == -1) {
        const auto ids = sectionsFor(s.category);
        std::vector<std::string> labels;
        labels.reserve(ids.size());
        for (const auto& id : ids) {
          labels.push_back(tr("sections." + id));
        }
        const auto it = std::ranges::find(ids, s.draft.sectionId);
        editor->addChild(labeled(
            scale, tr("subcategory"),
            ui::select({
                .options = std::move(labels),
                .selectedIndex = it == ids.end() ? 0 : static_cast<std::size_t>(it - ids.begin()),
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .width = kControlWidthWide * scale,
                .onSelectionChanged = [ids](std::size_t index, std::string_view) {
                  state().draft.sectionId = ids[index];
                },
            })
        ));
      }

      // Teclas: modificadores como botones y la tecla como texto. (No se graban pulsándolas: Hyprland
      // captura antes las combinaciones que ya tienen atajo.)
      auto keys = ui::row({.align = FlexAlign::End, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      for (const auto mod : kModifiers) {
        const std::string m(mod);
        const bool on = std::ranges::contains(s.draft.mods, m);
        keys->addChild(
            button(scale, m, on ? "check" : "", on ? ButtonVariant::Primary : ButtonVariant::Outline, [m, on] {
              auto& mods = state().draft.mods;
              if (on) {
                std::erase(mods, m);
              } else {
                mods.push_back(m);
              }
              rebuild();
            })
        );
      }
      keys->addChild(
          labeled(scale, tr("key"), textInput(scale, s.draft.key, 160, tr("key-placeholder"), [](const std::string& v) {
                    state().draft.key = v;
                  }))
      );
      editor->addChild(labeled(scale, tr("keys"), std::move(keys)));

      // Acción: de la shell, un comando o Lua.
      auto kinds = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      for (const auto kind : {BindAction::Kind::Shell, BindAction::Kind::Command, BindAction::Kind::Other}) {
        const bool on = s.draft.kind == kind;
        const std::string id = kind == BindAction::Kind::Shell
            ? "kind-shell"
            : (kind == BindAction::Kind::Command ? "kind-command" : "kind-lua");
        kinds->addChild(
            button(scale, tr(id), on ? "check" : "", on ? ButtonVariant::Primary : ButtonVariant::Outline, [kind] {
              state().draft.kind = kind;
              rebuild();
            })
        );
      }
      editor->addChild(labeled(scale, tr("action"), std::move(kinds)));

      auto actionRow =
          ui::row({.align = FlexAlign::End, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      if (s.draft.kind == BindAction::Kind::Shell) {
        const auto& actions = shellActions();
        const auto it = std::ranges::find(actions, s.draft.text);
        actionRow->addChild(
            ui::select({
                .options = actions,
                .selectedIndex = it == actions.end() ? std::optional<std::size_t>()
                                                     : std::optional<std::size_t>(it - actions.begin()),
                .clearSelection = it == actions.end(),
                .placeholder = tr("shell-pick"),
                .fontSize = Style::fontSizeCaption * scale,
                .controlHeight = Style::controlHeightSm * scale,
                .width = kControlWidthWide * scale,
                .onSelectionChanged = [](std::size_t index, std::string_view) {
                  state().draft.text = shellActions()[index];
                  rebuild();
                },
            })
        );
      }
      const std::string placeholder = s.draft.kind == BindAction::Kind::Shell
          ? tr("shell-placeholder")
          : (s.draft.kind == BindAction::Kind::Command ? tr("command-placeholder") : tr("lua-placeholder"));
      actionRow->addChild(textInput(scale, s.draft.text, 460, placeholder, [](const std::string& v) {
        state().draft.text = v;
      }));
      editor->addChild(std::move(actionRow));

      // Opciones.
      auto options = ui::row({.align = FlexAlign::End, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
      options->addChild(button(
          scale, tr("locked"), s.draft.locked ? "check" : "",
          s.draft.locked ? ButtonVariant::Primary : ButtonVariant::Outline, [] {
            state().draft.locked = !state().draft.locked;
            rebuild();
          }
      ));
      options->addChild(button(
          scale, tr("repeating"), s.draft.repeating ? "check" : "",
          s.draft.repeating ? ButtonVariant::Primary : ButtonVariant::Outline, [] {
            state().draft.repeating = !state().draft.repeating;
            rebuild();
          }
      ));
      options->addChild(
          labeled(scale, tr("description"), textInput(scale, s.draft.description, 320, {}, [](const std::string& v) {
                    state().draft.description = v;
                  }))
      );
      editor->addChild(std::move(options));

      auto actions = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
      );
      actions->addChild(button(scale, tr("cancel"), "circle-x", ButtonVariant::Ghost, [] {
        state().editing = -2;
        state().error.clear();
        rebuild();
      }));
      actions->addChild(button(scale, tr("save"), "device-floppy", ButtonVariant::Primary, [binds] { save(binds); }));
      editor->addChild(std::move(actions));
      parent.addChild(std::move(editor));
    }

  } // namespace

  void addSettingsBinds(Flex& content, SettingsBindsContext ctx) {
    auto& s = state();
    s.rebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;

    if (s.runtime.requestSocketPath().empty()) {
      Flex* body = addSettingsCard(content, tr("title"), scale);
      body->addChild(makeLabel(
          i18n::tr("settings.okaru.displays.hyprland-only"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return;
    }

    const auto binds = okaru::hypr::readAllBinds();
    std::map<std::string, int> keyUses;
    for (const auto& bind : binds) {
      ++keyUses[okaru::hypr::normalizeKeys(bind.keys)];
    }
    std::map<BindCategory, int> counts;
    for (const auto& bind : binds) {
      ++counts[okaru::hypr::categoryOf(bind)];
    }

    // Selector horizontal de categoría: cambia el contenido de la página.
    auto selector =
        ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
    for (const auto category : kCategories) {
      const bool selected = s.category == category;
      selector->addChild(button(
          scale, std::format("{} ({})", tr("category-" + std::string(categoryId(category))), counts[category]), "",
          selected ? ButtonVariant::Primary : ButtonVariant::Outline, [category] {
            auto& st = state();
            st.category = category;
            st.editing = -2;
            st.confirmDelete = -1;
            st.error.clear();
            rebuild();
          }
      ));
    }
    content.addChild(std::move(selector));

    if (!s.error.empty()) {
      auto error =
          makeLabel(s.error, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold);
      error->setMaxLines(4);
      content.addChild(std::move(error));
    }

    const std::string id(categoryId(s.category));
    Flex* card = addSettingsCard(content, tr("category-" + id), scale);
    auto description = makeLabel(
        tr("category-" + id + "-desc"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
    description->setMaxLines(3);
    card->addChild(std::move(description));

    // Crear y, en User, mover los atajos de apps que todavía están en binds.lua.
    auto actions =
        ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
    actions->addChild(button(scale, tr("new"), "plus", ButtonVariant::Outline, [] {
      auto& st = state();
      st.editing = -1;
      st.draft = Draft{};
      st.draft.mods = {"SUPER"};
      st.draft.kind = st.category == BindCategory::User
          ? BindAction::Kind::Command
          : ((st.category == BindCategory::Input || st.category == BindCategory::Other) ? BindAction::Kind::Other
                                                                                        : BindAction::Kind::Shell);
      {
        const auto ids = sectionsFor(st.category);
        st.draft.sectionId = ids.empty() ? std::string{} : ids.front();
      }
      st.error.clear();
      rebuild();
    }));
    std::vector<int> movable;
    if (s.category == BindCategory::User) {
      for (std::size_t i = 0; i < binds.size(); ++i) {
        if (binds[i].file == BindFile::Binds && okaru::hypr::categoryOf(binds[i]) == BindCategory::User) {
          movable.push_back(static_cast<int>(i));
        }
      }
      if (!movable.empty()) {
        actions->addChild(button(
            scale, std::format("{} ({})", tr("move-all"), movable.size()), "arrow-right", ButtonVariant::Outline,
            [binds, movable] { moveToUserBinds(binds, movable); }
        ));
      }
    }
    card->addChild(std::move(actions));
    if (s.category == BindCategory::User) {
      auto note = makeLabel(
          tr("user-logic-note"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      note->setMaxLines(3);
      card->addChild(std::move(note));
    }
    if (s.editing == -1) {
      addEditor(*card, scale, binds);
    }

    std::string lastSection;
    for (std::size_t i = 0; i < binds.size(); ++i) {
      const auto& bind = binds[i];
      if (okaru::hypr::categoryOf(bind) != s.category) {
        continue;
      }
      const int index = static_cast<int>(i);
      // Subtítulo con la sección del archivo (-- === Audio ===), como lo tiene organizado el usuario.
      if (!bind.section.empty() && bind.section != lastSection) {
        card->addChild(makeLabel(
            sectionLabel(bind.section), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Primary),
            FontWeight::Bold
        ));
        lastSection = bind.section;
      }
      const auto action = okaru::hypr::actionOf(bind);
      std::string what = action.text;
      if (bind.options.contains("description") && bind.options["description"].is_string()) {
        what = bind.options["description"].get<std::string>() + "  ·  " + what;
      }
      auto keys =
          makeLabel(bind.keys, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold);
      keys->setMinWidth(220.0F * scale);
      auto whatLabel = makeLabel(what, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      whatLabel->setMaxLines(2);
      whatLabel->setFlexGrow(1.0F);
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(std::move(keys));
      row->addChild(std::move(whatLabel));
      if (keyUses[okaru::hypr::normalizeKeys(bind.keys)] > 1) {
        row->addChild(makeLabel(
            tr("conflict"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold
        ));
      }
      if (s.confirmDelete == index) {
        row->addChild(makeLabel(
            tr("delete-confirm"), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface),
            FontWeight::Bold
        ));
        row->addChild(button(scale, tr("cancel"), "circle-x", ButtonVariant::Ghost, [] {
          state().confirmDelete = -1;
          rebuild();
        }));
        row->addChild(button(scale, tr("delete"), "trash", ButtonVariant::Destructive, [binds, index] {
          removeAt(binds, index);
        }));
      } else {
        if (bind.file == BindFile::Binds && s.category == BindCategory::User) {
          row->addChild(button(scale, tr("move"), "arrow-right", ButtonVariant::Ghost, [binds, index] {
            moveToUserBinds(binds, {index});
          }));
        }
        row->addChild(button(scale, {}, "edit", ButtonVariant::Ghost, [bind, index] {
          auto& st = state();
          st.editing = index;
          st.draft = draftFrom(bind);
          st.confirmDelete = -1;
          st.error.clear();
          rebuild();
        }));
        row->addChild(button(scale, {}, "trash", ButtonVariant::Ghost, [index] {
          state().confirmDelete = index;
          state().editing = -2;
          rebuild();
        }));
      }
      card->addChild(std::move(row));
      if (s.editing == index) {
        addEditor(*card, scale, binds);
      }
    }
  }

} // namespace settings
