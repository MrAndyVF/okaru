#include "config/config_types.h"
#include "scripting/plugin_source_paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_source_paths_test: {}", message);
    }
    return condition;
  }

  bool expectPath(const std::filesystem::path& actual, const std::filesystem::path& expected, const char* message) {
    if (actual != expected) {
      std::println(
          stderr, "plugin_source_paths_test: {}\n  actual:   {}\n  expected: {}", message, actual.string(),
          expected.string()
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  ::setenv("OKARU_STATE_HOME", "/tmp/okaru-path-test-state", 1);
  ::setenv("OKARU_DATA_HOME", "/tmp/okaru-path-test-data", 1);

  const PluginSourceConfig gitSource{
      .kind = PluginSourceKind::Git,
      .name = "official",
      .location = "https://example.invalid/plugins.git",
  };
  const PluginSourceConfig pathSource{
      .kind = PluginSourceKind::Path,
      .name = "dev",
      .location = "~/dev/okaru-plugins",
  };

  const std::filesystem::path stateRoot = "/tmp/okaru-path-test-state/okaru";
  bool ok = true;
  ok = expectPath(
           scripting::plugin_paths::localSourceRoot(), "/tmp/okaru-path-test-data/okaru/plugins",
           "local source root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::sourceStorageRoot(gitSource), stateRoot / "plugins/sources/official",
           "git source storage root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitRepoRoot(gitSource), stateRoot / "plugins/sources/official/repo", "git repo root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitMaterializedRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git materialized root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::registryRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git registry root"
       )
      && ok;
  // OKARU: la única fuente por defecto es la tienda okaru.
  ok = expect(isDefaultPluginSourceName("okaru"), "okaru source should be protected as a default source") && ok;
  ok = expect(!isDefaultPluginSourceName("official"), "okaru sources are no longer defaults") && ok;
  ok = expect(!isDefaultPluginSourceName("dev"), "custom source should not be protected as a default source") && ok;
  ok = expect(
           scripting::plugin_paths::registryRoot(pathSource).string().ends_with("/dev/okaru-plugins"),
           "path source registry root expands user path"
       )
      && ok;
  ok = expect(
           scripting::plugin_paths::pathIsInside("/tmp/okaru/a/b", "/tmp/okaru"),
           "child path should be inside parent"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/okaru", "/tmp/okaru"),
           "parent path must not count as inside itself"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/okaru-other/a", "/tmp/okaru"),
           "sibling prefix must not count as inside parent"
       )
      && ok;

  return ok ? 0 : 1;
}
