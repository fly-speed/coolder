package io.coolder.app;

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.stream.*;

/** Java file confinement and optimistic edits; external roots require app registration. */
final class Workspace {
  final Path root;
  final List<Object> projects;

  Workspace(Path root, List<Object> projects) {
    this.root = root;
    this.projects = projects;
  }

  static String normalized(String p, boolean empty) {
    if (p.isEmpty() && empty) return "";
    if (p.isEmpty() || p.startsWith("/") || p.contains("\\") || p.indexOf('\0') >= 0)
      throw new App.Error(400, "invalid path");
    for (String c : p.split("/", -1))
      if (c.isEmpty()
          || c.equals(".")
          || c.equals("..")
          || c.equals(".libai_agent")
          || c.equals(".git")
          || c.equals(".env")
          || c.equals("secrets")
          || c.equals("private")) throw new App.Error(400, "private or invalid path");
    return p;
  }

  Path resolve(String p, boolean empty) throws Exception {
    normalized(p, empty);
    Path base = root;
    String relative = p;
    for (Object v : projects) {
      Map<String, Object> pr = Json.map(v);
      String alias = Json.text(pr, "path");
      if (!Json.text(pr, "external").isEmpty() && (p.equals(alias) || p.startsWith(alias + "/"))) {
        base = Paths.get(Json.text(pr, "external"));
        relative = p.equals(alias) ? "" : p.substring(alias.length() + 1);
        break;
      }
    }
    Path dest = base.resolve(relative).normalize();
    if (!dest.startsWith(base)) throw new App.Error(400, "outside workspace");
    Path current = base;
    if (Files.isSymbolicLink(base)) throw new App.Error(400, "symlink root");
    for (Path c : base.relativize(dest)) {
      current = current.resolve(c);
      if (Files.isSymbolicLink(current)) throw new App.Error(400, "symlink path");
    }
    return dest;
  }

  Object read(String p) throws Exception {
    Path f = resolve(p, false);
    if (!Files.isRegularFile(f) || Files.size(f) > 1024 * 1024)
      throw new App.Error(400, "invalid file or file too large");
    byte[] bytes = Files.readAllBytes(f);
    return Json.obj(
        "ok",
        true,
        "path",
        p,
        "content",
        new String(bytes, StandardCharsets.UTF_8),
        "sha256",
        Store.hash(bytes),
        "truncated",
        false);
  }

  Object list(String p) throws Exception {
    Path d = resolve(p, true);
    if (!Files.isDirectory(d)) throw new App.Error(400, "directory missing");
    List<Object> items = new ArrayList<>();
    try (Stream<Path> s = Files.list(d)) {
      for (Path f : s.sorted().collect(Collectors.toList())) {
        String name = f.getFileName().toString();
        if (name.startsWith(".") || Files.isSymbolicLink(f)) continue;
        items.add(
            Json.obj(
                "name",
                name,
                "path",
                p.isEmpty() ? name : p + "/" + name,
                "directory",
                Files.isDirectory(f),
                "size",
                Files.isDirectory(f) ? 0 : Files.size(f)));
      }
    }
    return Json.obj("ok", true, "path", p, "entries", items);
  }

  Object search(String p, String q) throws Exception {
    Path d = resolve(p, true);
    List<Object> matches = new ArrayList<>();
    try (Stream<Path> s = Files.walk(d, 12)) {
      for (Path f : s.limit(10000).collect(Collectors.toList())) {
        String rel = root.relativize(f).toString().replace('\\', '/');
        if (Files.isRegularFile(f) && !Files.isSymbolicLink(f) && Files.size(f) < 1024 * 1024) {
          try {
            resolve(rel, false);
          } catch (App.Error e) {
            continue;
          }
          String[] lines = new String(Files.readAllBytes(f), StandardCharsets.UTF_8).split("\n");
          for (int i = 0; i < lines.length && matches.size() < 500; i++)
            if (lines[i].contains(q))
              matches.add(Json.obj("path", rel, "line", i + 1, "text", lines[i]));
        }
      }
    }
    return Json.obj("ok", true, "matches", matches, "truncated", matches.size() >= 500);
  }

  Object asset(String p) throws Exception {
    Path f = resolve(p, false);
    String ext = p.substring(p.lastIndexOf('.') + 1).toLowerCase(Locale.ROOT);
    Map<String, String> types = new HashMap<>();
    types.put("png", "image/png");
    types.put("jpg", "image/jpeg");
    types.put("jpeg", "image/jpeg");
    types.put("gif", "image/gif");
    types.put("webp", "image/webp");
    types.put("svg", "image/svg+xml");
    types.put("ico", "image/x-icon");
    types.put("woff", "font/woff");
    types.put("woff2", "font/woff2");
    types.put("ttf", "font/ttf");
    if (!types.containsKey(ext) || !Files.isRegularFile(f) || Files.size(f) > 2 * 1024 * 1024)
      throw new App.Error(400, "invalid preview asset");
    return Json.obj(
        "ok",
        true,
        "mime",
        types.get(ext),
        "base64",
        Base64.getEncoder().encodeToString(Files.readAllBytes(f)));
  }

  void replace(String p, String expected, String content) throws Exception {
    Path f = resolve(p, false);
    if (!Files.isRegularFile(f) || !Store.equal(Store.hash(Files.readAllBytes(f)), expected))
      throw new App.Error(409, "file changed");
    Files.write(f, content.getBytes(StandardCharsets.UTF_8));
  }
}
