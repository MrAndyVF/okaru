#include "apps/pdf/pdf_document.h"

#include <algorithm>
#include <cairo.h>
#include <charconv>
#include <cmath>
#include <glib.h>
#include <poppler.h>

namespace okaru::pdf {

  namespace {

    std::string takeString(gchar* value) {
      std::string out = value != nullptr ? value : "";
      g_free(value);
      return out;
    }

    std::string formatDate(GDateTime* date) {
      if (date == nullptr) {
        return {};
      }
      gchar* text = g_date_time_format(date, "%Y-%m-%d %H:%M");
      g_date_time_unref(date);
      return takeString(text);
    }

    // poppler da la posición de búsquedas y enlaces con el origen abajo; aquí todo va con el origen arriba.
    Rect flipped(const PopplerRectangle& r, double pageHeight) {
      return {
          std::min(r.x1, r.x2), pageHeight - std::max(r.y1, r.y2), std::max(r.x1, r.x2),
          pageHeight - std::min(r.y1, r.y2)
      };
    }

    // Página (desde 0) de un destino, resolviendo los destinos con nombre.
    int destPage(PopplerDocument* doc, PopplerDest* dest) {
      if (dest == nullptr) {
        return -1;
      }
      if (dest->type == POPPLER_DEST_NAMED) {
        PopplerDest* named = poppler_document_find_dest(doc, dest->named_dest);
        const int page = named != nullptr ? named->page_num - 1 : -1;
        if (named != nullptr) {
          poppler_dest_free(named);
        }
        return page;
      }
      return dest->page_num - 1;
    }

    void walkOutline(PopplerDocument* doc, PopplerIndexIter* iter, int depth, std::vector<OutlineItem>& out) {
      do {
        PopplerAction* action = poppler_index_iter_get_action(iter);
        if (action != nullptr) {
          OutlineItem item{.depth = depth};
          if (action->any.title != nullptr) {
            item.title = action->any.title;
          }
          if (action->type == POPPLER_ACTION_GOTO_DEST) {
            item.page = destPage(doc, action->goto_dest.dest);
          }
          out.push_back(std::move(item));
          poppler_action_free(action);
        }
        if (PopplerIndexIter* child = poppler_index_iter_get_child(iter); child != nullptr) {
          walkOutline(doc, child, depth + 1, out);
          poppler_index_iter_free(child);
        }
      } while (poppler_index_iter_next(iter));
    }

  } // namespace

  Document::Document(PopplerDocument* doc) : m_doc(doc) {}

  Document::~Document() {
    if (m_doc != nullptr) {
      g_object_unref(m_doc);
    }
  }

  std::expected<std::unique_ptr<Document>, OpenError>
  Document::open(const std::filesystem::path& path, const std::string& password) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri == nullptr) {
      return std::unexpected(OpenError::Damaged);
    }
    GError* error = nullptr;
    PopplerDocument* doc = poppler_document_new_from_file(uri, password.empty() ? nullptr : password.c_str(), &error);
    g_free(uri);
    if (doc == nullptr) {
      const bool encrypted =
          error != nullptr && error->domain == POPPLER_ERROR && error->code == POPPLER_ERROR_ENCRYPTED;
      g_clear_error(&error);
      if (encrypted) {
        return std::unexpected(password.empty() ? OpenError::NeedsPassword : OpenError::WrongPassword);
      }
      return std::unexpected(OpenError::Damaged);
    }
    return std::unique_ptr<Document>(new Document(doc));
  }

  int Document::pageCount() const { return poppler_document_get_n_pages(m_doc); }

  PageSize Document::pageSize(int page) const {
    PageSize size;
    if (PopplerPage* p = poppler_document_get_page(m_doc, page); p != nullptr) {
      poppler_page_get_size(p, &size.width, &size.height);
      g_object_unref(p);
    }
    return size;
  }

  std::string Document::pageLabel(int page) const {
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr) {
      return {};
    }
    std::string label = takeString(poppler_page_get_label(p));
    g_object_unref(p);
    // Muchos PDF ponen como etiqueta el mismo número: no aporta nada.
    return label == std::to_string(page + 1) ? std::string() : label;
  }

  std::vector<OutlineItem> Document::outline() const {
    std::vector<OutlineItem> out;
    if (PopplerIndexIter* iter = poppler_index_iter_new(m_doc); iter != nullptr) {
      walkOutline(m_doc, iter, 0, out);
      poppler_index_iter_free(iter);
    }
    return out;
  }

  std::vector<Link> Document::links(int page) const {
    std::vector<Link> out;
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr) {
      return out;
    }
    double width = 0.0;
    double height = 0.0;
    poppler_page_get_size(p, &width, &height);
    GList* mapping = poppler_page_get_link_mapping(p);
    for (GList* it = mapping; it != nullptr; it = it->next) {
      const auto* link = static_cast<PopplerLinkMapping*>(it->data);
      Link item{.area = flipped(link->area, height)};
      if (link->action == nullptr) {
        continue;
      }
      if (link->action->type == POPPLER_ACTION_GOTO_DEST) {
        item.page = destPage(m_doc, link->action->goto_dest.dest);
      } else if (link->action->type == POPPLER_ACTION_URI && link->action->uri.uri != nullptr) {
        item.uri = link->action->uri.uri;
      } else {
        continue;
      }
      out.push_back(std::move(item));
    }
    poppler_page_free_link_mapping(mapping);
    g_object_unref(p);
    return out;
  }

  std::vector<FormField> Document::formFields(int page) const {
    std::vector<FormField> out;
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr) {
      return out;
    }
    double width = 0.0;
    double height = 0.0;
    poppler_page_get_size(p, &width, &height);
    GList* mapping = poppler_page_get_form_field_mapping(p);
    for (GList* it = mapping; it != nullptr; it = it->next) {
      const auto* item = static_cast<PopplerFormFieldMapping*>(it->data);
      PopplerFormField* field = item->field;
      FormField f{.id = poppler_form_field_get_id(field), .area = flipped(item->area, height)};
      f.readOnly = poppler_form_field_is_read_only(field) != FALSE;
      switch (poppler_form_field_get_field_type(field)) {
      case POPPLER_FORM_FIELD_TEXT:
        f.kind = FormField::Kind::Text;
        f.text = takeString(poppler_form_field_text_get_text(field));
        f.multiline = poppler_form_field_text_get_text_type(field) == POPPLER_FORM_TEXT_MULTILINE;
        f.password = poppler_form_field_text_is_password(field) != FALSE;
        f.maxLength = poppler_form_field_text_get_max_len(field);
        if (f.maxLength <= 0) {
          f.maxLength = -1;
        }
        break;
      case POPPLER_FORM_FIELD_BUTTON: {
        const PopplerFormButtonType type = poppler_form_field_button_get_button_type(field);
        f.kind = type == POPPLER_FORM_BUTTON_CHECK ? FormField::Kind::Check
            : type == POPPLER_FORM_BUTTON_RADIO    ? FormField::Kind::Radio
                                                   : FormField::Kind::Other;
        f.checked = poppler_form_field_button_get_state(field) != FALSE;
        break;
      }
      case POPPLER_FORM_FIELD_CHOICE:
        f.kind = FormField::Kind::Choice;
        for (int i = 0; i < poppler_form_field_choice_get_n_items(field); ++i) {
          f.choices.push_back(takeString(poppler_form_field_choice_get_item(field, i)));
          if (poppler_form_field_choice_is_item_selected(field, i) != FALSE && f.selected < 0) {
            f.selected = i;
          }
        }
        break;
      default:
        break;
      }
      out.push_back(std::move(f));
    }
    poppler_page_free_form_field_mapping(mapping);
    g_object_unref(p);
    return out;
  }

  bool Document::apply(const Edit& edit) {
    if (edit.kind == Edit::Kind::Highlight || edit.kind == Edit::Kind::Underline) {
      PopplerPage* p = poppler_document_get_page(m_doc, edit.page);
      if (p == nullptr || edit.rects.empty()) {
        if (p != nullptr) {
          g_object_unref(p);
        }
        return false;
      }
      double width = 0.0;
      double height = 0.0;
      poppler_page_get_size(p, &width, &height);
      // poppler quiere el origen abajo: se voltea. Cada línea es un cuadrilátero; el rectángulo, el que los abarca.
      GArray* quads =
          g_array_sized_new(FALSE, FALSE, sizeof(PopplerQuadrilateral), static_cast<guint>(edit.rects.size()));
      PopplerRectangle bounds{1e9, 1e9, -1e9, -1e9};
      for (const Rect& r : edit.rects) {
        const double top = height - r.y0;
        const double bottom = height - r.y1;
        PopplerQuadrilateral q{{r.x0, top}, {r.x1, top}, {r.x0, bottom}, {r.x1, bottom}};
        g_array_append_val(quads, q);
        bounds.x1 = std::min(bounds.x1, r.x0);
        bounds.y1 = std::min(bounds.y1, bottom);
        bounds.x2 = std::max(bounds.x2, r.x1);
        bounds.y2 = std::max(bounds.y2, top);
      }
      PopplerAnnot* annot = edit.kind == Edit::Kind::Highlight
          ? poppler_annot_text_markup_new_highlight(m_doc, &bounds, quads)
          : poppler_annot_text_markup_new_underline(m_doc, &bounds, quads);
      g_array_unref(quads);
      // Amarillo de resaltador y rojo de corrección, como en los demás lectores.
      PopplerColor color =
          edit.kind == Edit::Kind::Highlight ? PopplerColor{65535, 59000, 0} : PopplerColor{56000, 8000, 8000};
      poppler_annot_set_color(annot, &color);
      poppler_page_add_annot(p, annot);
      g_object_unref(annot);
      g_object_unref(p);
      return true;
    }
    PopplerFormField* field = poppler_document_get_form_field(m_doc, edit.field);
    if (field == nullptr) {
      return false;
    }
    bool ok = poppler_form_field_is_read_only(field) == FALSE;
    if (ok) {
      switch (edit.kind) {
      case Edit::Kind::Text:
        poppler_form_field_text_set_text(field, edit.text.c_str());
        break;
      case Edit::Kind::Check:
        poppler_form_field_button_set_state(field, edit.checked ? TRUE : FALSE);
        break;
      case Edit::Kind::Choice:
        poppler_form_field_choice_unselect_all(field);
        if (edit.choice >= 0) {
          poppler_form_field_choice_select_item(field, edit.choice);
        }
        break;
      default:
        ok = false;
        break;
      }
    }
    g_object_unref(field);
    return ok;
  }

  bool Document::save(const std::filesystem::path& path, std::string& error) const {
    // Nunca encima del archivo abierto: poppler lo sigue leyendo. Temporal y luego rename (el abierto sigue vivo).
    const std::filesystem::path temp = path.parent_path() / ("." + path.filename().string() + ".okaru-save");
    GError* gerror = nullptr;
    gchar* uri = g_filename_to_uri(temp.c_str(), nullptr, &gerror);
    const bool written = uri != nullptr && poppler_document_save(m_doc, uri, &gerror) != FALSE;
    g_free(uri);
    if (!written) {
      error = gerror != nullptr ? gerror->message : "poppler";
      g_clear_error(&gerror);
      std::error_code ec;
      std::filesystem::remove(temp, ec);
      return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      std::filesystem::permissions(temp, std::filesystem::status(path, ec).permissions(), ec);
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
      error = ec.message();
      std::filesystem::remove(temp, ec);
      return false;
    }
    return true;
  }

  std::vector<Rect> Document::find(int page, std::string_view text, bool caseSensitive, bool wholeWords) const {
    std::vector<Rect> out;
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr || text.empty()) {
      if (p != nullptr) {
        g_object_unref(p);
      }
      return out;
    }
    double width = 0.0;
    double height = 0.0;
    poppler_page_get_size(p, &width, &height);
    int flags = POPPLER_FIND_IGNORE_DIACRITICS | POPPLER_FIND_MULTILINE;
    flags |= caseSensitive ? POPPLER_FIND_CASE_SENSITIVE : 0;
    flags |= wholeWords ? POPPLER_FIND_WHOLE_WORDS_ONLY : 0;
    const std::string needle(text);
    GList* matches = poppler_page_find_text_with_options(p, needle.c_str(), static_cast<PopplerFindFlags>(flags));
    for (GList* it = matches; it != nullptr; it = it->next) {
      auto* rect = static_cast<PopplerRectangle*>(it->data);
      out.push_back(flipped(*rect, height));
      poppler_rectangle_free(rect);
    }
    g_list_free(matches);
    g_object_unref(p);
    return out;
  }

  TextLayout Document::textLayout(int page) const {
    TextLayout layout;
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr) {
      return layout;
    }
    layout.text = takeString(poppler_page_get_text(p));
    PopplerRectangle* rects = nullptr;
    guint count = 0;
    if (poppler_page_get_text_layout(p, &rects, &count)) {
      // Un rectángulo por carácter del texto (los saltos de línea también cuentan).
      std::size_t byte = 0;
      for (guint i = 0; i < count && byte < layout.text.size(); ++i) {
        layout.offsets.push_back(byte);
        layout.chars.push_back({rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2});
        const gchar* next = g_utf8_next_char(layout.text.c_str() + byte);
        byte = static_cast<std::size_t>(next - layout.text.c_str());
      }
      g_free(rects);
    }
    g_object_unref(p);
    return layout;
  }

  Metadata Document::metadata() const {
    Metadata meta;
    meta.title = takeString(poppler_document_get_title(m_doc));
    meta.author = takeString(poppler_document_get_author(m_doc));
    meta.subject = takeString(poppler_document_get_subject(m_doc));
    meta.keywords = takeString(poppler_document_get_keywords(m_doc));
    meta.creator = takeString(poppler_document_get_creator(m_doc));
    meta.producer = takeString(poppler_document_get_producer(m_doc));
    meta.created = formatDate(poppler_document_get_creation_date_time(m_doc));
    meta.modified = formatDate(poppler_document_get_modification_date_time(m_doc));
    meta.version = takeString(poppler_document_get_pdf_version_string(m_doc));
    return meta;
  }

  RenderedPage Document::render(int page, double scale, int rotation, bool invert) const {
    RenderedPage out;
    PopplerPage* p = poppler_document_get_page(m_doc, page);
    if (p == nullptr) {
      return out;
    }
    double width = 0.0;
    double height = 0.0;
    poppler_page_get_size(p, &width, &height);
    rotation = ((rotation % 360) + 360) % 360;
    const bool sideways = rotation == 90 || rotation == 270;
    out.width = std::max(1, static_cast<int>(std::ceil((sideways ? height : width) * scale)));
    out.height = std::max(1, static_cast<int>(std::ceil((sideways ? width : height) * scale)));

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out.width, out.height);
    cairo_t* cr = cairo_create(surface);
    // Fondo blanco: muchos PDF no pintan el papel.
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    switch (rotation) {
    case 90:
      cairo_translate(cr, height, 0.0);
      break;
    case 180:
      cairo_translate(cr, width, height);
      break;
    case 270:
      cairo_translate(cr, 0.0, width);
      break;
    default:
      break;
    }
    cairo_rotate(cr, rotation * G_PI / 180.0);
    poppler_page_render(p, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    g_object_unref(p);

    // ARGB32 de cairo (BGRA en memoria, opaco) → RGBA.
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    out.rgba.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4U);
    for (int y = 0; y < out.height; ++y) {
      const unsigned char* row = data + static_cast<std::ptrdiff_t>(y) * stride;
      std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) * 4U;
      for (int x = 0; x < out.width; ++x) {
        const std::uint8_t b = row[x * 4 + 0];
        const std::uint8_t g = row[x * 4 + 1];
        const std::uint8_t r = row[x * 4 + 2];
        dst[x * 4 + 0] = invert ? static_cast<std::uint8_t>(255 - r) : r;
        dst[x * 4 + 1] = invert ? static_cast<std::uint8_t>(255 - g) : g;
        dst[x * 4 + 2] = invert ? static_cast<std::uint8_t>(255 - b) : b;
        dst[x * 4 + 3] = 255;
      }
    }
    cairo_surface_destroy(surface);
    return out;
  }

  std::vector<int> parsePageRange(std::string_view text, int pageCount) {
    std::vector<int> pages;
    const auto number = [](std::string_view s, int& value) {
      while (!s.empty() && s.front() == ' ') {
        s.remove_prefix(1);
      }
      while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
      }
      return !s.empty() && std::from_chars(s.data(), s.data() + s.size(), value).ec == std::errc{};
    };
    while (!text.empty()) {
      const auto comma = text.find(',');
      const std::string_view part = text.substr(0, comma);
      text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
      const auto dash = part.find('-');
      int from = 0;
      int to = 0;
      if (dash == std::string_view::npos) {
        if (!number(part, from)) {
          return {};
        }
        to = from;
      } else {
        // "3-" = hasta el final; "-3" = desde el principio.
        const std::string_view left = part.substr(0, dash);
        const std::string_view right = part.substr(dash + 1);
        from = 1;
        to = pageCount;
        if (left.find_first_not_of(' ') != std::string_view::npos && !number(left, from)) {
          return {};
        }
        if (right.find_first_not_of(' ') != std::string_view::npos && !number(right, to)) {
          return {};
        }
      }
      if (from < 1 || to > pageCount || from > to) {
        return {};
      }
      for (int page = from; page <= to; ++page) {
        pages.push_back(page - 1);
      }
    }
    std::ranges::sort(pages);
    pages.erase(std::ranges::unique(pages).begin(), pages.end());
    return pages;
  }

} // namespace okaru::pdf
