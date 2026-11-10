// OKARU: lista de URIs que llega al soltar archivos sobre una app.
#include "apps/common/file_drop.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
  using okaru::apps::parseUriList;
  using Paths = std::vector<std::string>;
  assert(
      parseUriList("file:///home/a/foto%20uno.png\r\nfile:///tmp/b.pdf\r\n")
      == (Paths{"/home/a/foto uno.png", "/tmp/b.pdf"})
  );
  assert(parseUriList("# comentario\n\nhttps://x.org/v.mp4") == Paths{"https://x.org/v.mp4"});
  assert(parseUriList("file://localhost/etc/hosts\n") == Paths{"/etc/hosts"});
  assert(parseUriList("").empty());
  return 0;
}
