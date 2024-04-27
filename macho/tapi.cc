// On macOS, you can pass a test file describing a dylib instead of an
// actual dylib file to link against a dynamic library. Such text file
// should be in the YAML format and contains dylib's exported symbols
// as well as the file's various attributes. The extension of the text
// file is `.tbd`.
//
// .tbd files allows users to link against a library without
// distributing the binary of the library file itself.
//
// This file contains functions to parse the .tbd file.

#include "mold.h"

#include <optional>
#include <regex>
#include <unordered_set>

namespace mold::macho {

template <typename T, typename U>
static void merge(std::set<T> &a, U &&b) {
  a.insert(b.begin(), b.end());
}

static std::vector<YamlNode>
get_vector(YamlNode &node, std::string_view key) {
  if (auto *map = std::get_if<std::map<std::string_view, YamlNode>>(&node.data))
    if (auto it = map->find(key); it != map->end())
      if (auto *vec = std::get_if<std::vector<YamlNode>>(&it->second.data))
        return *vec;
  return {};
}

static std::vector<std::string_view>
get_string_vector(YamlNode &node, std::string_view key) {
  std::vector<std::string_view> vec;
  for (YamlNode &mem : get_vector(node, key))
    if (auto val = std::get_if<std::string_view>(&mem.data))
      vec.push_back(*val);
  return vec;
}

static std::optional<std::string_view>
get_string(YamlNode &node, std::string_view key) {
  if (auto *map = std::get_if<std::map<std::string_view, YamlNode>>(&node.data))
    if (auto it = map->find(key); it != map->end())
      if (auto *str = std::get_if<std::string_view>(&it->second.data))
        return *str;
  return {};
}

static bool contains(const std::vector<YamlNode> &vec, std::string_view key) {
  for (const YamlNode &mem : vec)
    if (auto val = std::get_if<std::string_view>(&mem.data))
      if (*val == key)
        return true;
  return false;
}

template <typename E>
static bool match_arch(Context<E> &ctx, const std::vector<YamlNode> &vec) {
  static_assert(is_arm<E> || is_x86<E>);
  std::string arch = is_arm<E> ? "arm64" : "x86_64";

  for (const YamlNode &mem : vec)
    if (auto val = std::get_if<std::string_view>(&mem.data))
      if (*val == arch || val->starts_with(arch + "-"))
        return true;
  return false;
}

template <typename E>
static std::optional<TextDylib>
to_tbd(Context<E> &ctx, YamlNode &node, std::string_view filename) {
  // Before v4 of .tbd, the key is actually "archs" instead of "targets".
  // There are other differences, and detection should be done through tags,
  // however there's little benefit and needless overhead.
  const char* targets_key = "targets";
  if (!match_arch(ctx, get_vector(node, targets_key))) {
    targets_key = "archs";
  }
  if (!match_arch(ctx, get_vector(node, targets_key))) {
    return {};
  }

  if (ctx.arg.application_extension &&
      contains(get_vector(node, "flags"), "not_app_extension_safe"))
    Warn(ctx) << "linking against a dylib which is not safe for use in "
              << "application extensions: " << filename;

  TextDylib tbd;

  if (auto val = get_string(node, "install-name"))
    tbd.install_name = *val;

  for (YamlNode &mem : get_vector(node, "reexported-libraries"))
    if (match_arch(ctx, get_vector(mem, targets_key)))
      append(tbd.reexported_libs, get_string_vector(mem, "libraries"));

  auto concat = [&](const std::string &x, std::string_view y) {
    return save_string(ctx, x + std::string(y));
  };

  for (std::string_view key : {"exports", "reexports"}) {
    for (YamlNode &mem : get_vector(node, key)) {
      if (match_arch(ctx, get_vector(mem, targets_key))) {
        merge(tbd.exports, get_string_vector(mem, "symbols"));
        merge(tbd.weak_exports, get_string_vector(mem, "weak-symbols"));

        for (std::string_view s : get_string_vector(mem, "objc-classes")) {
          tbd.exports.insert(concat("_OBJC_CLASS_$_", s));
          tbd.exports.insert(concat("_OBJC_METACLASS_$_", s));
        }

        for (std::string_view s : get_string_vector(mem, "objc-eh-types"))
          tbd.exports.insert(concat("_OBJC_EHTYPE_$_", s));

        for (std::string_view s : get_string_vector(mem, "objc-ivars"))
          tbd.exports.insert(concat("_OBJC_IVAR_$_", s));
      }
    }
  }

  return tbd;
}

static std::vector<std::string_view> split_string(std::string_view str, i64 max) {
  std::vector<std::string_view> vec;
  while (vec.size() < max) {
    size_t pos = str.find('$');
    if (pos == str.npos) {
      vec.push_back(str);
      return vec;
    }

    vec.push_back(str.substr(0, pos));
    str = str.substr(pos + 1);
  }
  return vec;
}

// Dylib can contain special symbols whose name starts with "$ld$".
// Such symbols aren't actually symbols but linker directives.
// We interpret such symbols in this function.
template <typename E>
static void
interpret_ld_symbols(Context<E> &ctx, TextDylib &tbd, std::string_view filename) {
  std::set<std::string_view> syms;
  std::unordered_set<std::string_view> hidden_syms;

  for (std::string_view str : tbd.exports) {
    // $ld$previous$ symbol replaces the default install name with a
    // specified one if the platform OS version is in a specified range.
    if (remove_prefix(str, "$ld$previous$")) {
      std::vector<std::string_view> args = split_string(str, 6);
      if (args.size() != 6)
        Fatal(ctx) << filename << ": malformed symbol: $ld$previous$" << str;

      std::string_view install_name = args[0];
      i64 platform = std::stoi(std::string(args[2]));
      VersionTriple min_version = parse_version(ctx, args[3]);
      VersionTriple max_version = parse_version(ctx, args[4]);
      std::string_view symbol_name = args[5];

      if (!symbol_name.empty()) {
        // ld64 source seems to have implemented a feature to give an
        // alternative install name for a matching symbol, but it didn't
        // work in practice (or I may be using the feature in a wrong way.)
        // Ignore such symbol for now.
        continue;
      }

      if (platform == ctx.arg.platform &&
          min_version <= ctx.arg.platform_min_version &&
          ctx.arg.platform_min_version < max_version) {
        tbd.install_name = install_name;
      }
      continue;
    }

    // $ld$add$os_version$symbol adds a symbol if the given OS version
    // matches.
    if (remove_prefix(str, "$ld$add$os")) {
      std::vector<std::string_view> args = split_string(str, 2);
      if (args.size() != 2)
        Fatal(ctx) << filename << ": malformed symbol: $ld$add$os" << str;

      VersionTriple version = parse_version(ctx, args[0]);
      if (ctx.arg.platform_min_version == version)
        syms.insert(args[1]);
      continue;
    }

    // $ld$hide$os_version$symbol hides a symbol if the given OS version
    // matches.
    if (remove_prefix(str, "$ld$hide$os")) {
      std::vector<std::string_view> args = split_string(str, 2);
      if (args.size() != 2)
        Fatal(ctx) << filename << ": malformed symbol: $ld$hide$os" << str;

      VersionTriple version = parse_version(ctx, args[0]);
      if (ctx.arg.platform_min_version == version)
        hidden_syms.insert(args[1]);
      continue;
    }

    // $ld$install_name$os_version$name changes the install name to a
    // given name.
    if (remove_prefix(str, "$ld$install_name$os")) {
      std::vector<std::string_view> args = split_string(str, 2);
      if (args.size() != 2)
        Fatal(ctx) << filename << ": malformed symbol: $ld$install_name$os" << str;

      VersionTriple version = parse_version(ctx, args[0]);
      if (ctx.arg.platform_min_version == version)
        tbd.install_name = args[1];
      continue;
    }
  }

  for (std::string_view str : tbd.exports)
    if (!str.starts_with("$ld$") && !hidden_syms.contains(str))
      syms.insert(str);

  tbd.exports = std::move(syms);
}

// A single YAML file may contain multiple text dylibs. The first text
// dylib is the main file followed by optional other text dylibs for
// re-exported libraries.
//
// This fucntion squashes multiple text dylibs into a single text dylib
// by copying symbols of re-exported text dylibs to the main text dylib.
template <typename E>
static TextDylib squash(Context<E> &ctx, std::span<TextDylib> tbds) {
  std::unordered_map<std::string_view, TextDylib> map;
  std::vector<std::string_view> remainings;

  TextDylib main = std::move(tbds[0]);
  for (TextDylib &tbd : tbds.subspan(1))
    map[tbd.install_name] = std::move(tbd);

  std::function<void(TextDylib &)> visit = [&](TextDylib &tbd) {
    for (std::string_view lib : tbd.reexported_libs) {
      auto it = map.find(lib);

      if (it != map.end()) {
        TextDylib &child = it->second;
        merge(main.exports, child.exports);
        merge(main.weak_exports, child.weak_exports);
        visit(child);
      } else {
        remainings.push_back(lib);
      }
    }
  };

  visit(main);
  main.reexported_libs = remainings;
  return main;
}

template <typename E>
static std::string_view replace_crlf(Context<E> &ctx, std::string_view str) {
  if (str.find('\r') == str.npos)
    return str;

  std::string buf;
  for (char c : str)
    if (c != '\r')
      buf += c;
  return save_string(ctx, buf);
}

template <typename E>
TextDylib parse_tbd(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  std::string_view contents = mf->get_contents();
  contents = replace_crlf(ctx, contents);

  std::variant<std::vector<YamlNode>, YamlError> res = parse_yaml(contents);

  if (YamlError *err = std::get_if<YamlError>(&res)) {
    i64 lineno = std::count(contents.begin(), contents.begin() + err->pos, '\n');
    Fatal(ctx) << mf->name << ":" << (lineno + 1)
               << ": YAML parse error: " << err->msg;
  }

  std::vector<YamlNode> &nodes = std::get<std::vector<YamlNode>>(res);
  if (nodes.empty())
    Fatal(ctx) << mf->name << ": malformed TBD file";

  std::vector<TextDylib> vec;
  for (YamlNode &node : nodes)
    if (std::optional<TextDylib> dylib = to_tbd(ctx, node, mf->name))
      vec.push_back(*dylib);

  for (TextDylib &tbd : vec)
    interpret_ld_symbols(ctx, tbd, mf->name);

  return squash(ctx, vec);
}

using E = MOLD_TARGET;

template TextDylib parse_tbd(Context<E> &, MappedFile<Context<E>> *);

} // namespace mold::macho
