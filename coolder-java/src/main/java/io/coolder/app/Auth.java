package io.coolder.app;

import java.nio.file.*;
import java.util.*;

/** Java-owned account database, session tokens, throttling and preferences. */
final class Auth {
  final Path data, workspace, html;
  final String cookie = "coolder_java_session";
  final Map<String, Object> accounts;
  final Map<String, String> tokens = new HashMap<>();
  final Map<String, Long> expirations = new HashMap<>();
  final Map<String, Integer> attempts = new HashMap<>();
  final Map<String, Long> attemptedAt = new HashMap<>();

  Auth(Path data, Path workspace, Path html) throws Exception {
    this.data = data;
    this.workspace = workspace;
    this.html = html;
    Store.privateDir(data.resolve("auth"));
    try {
      accounts = AccountStore.load(data.resolve("auth/accounts.v1"));
    } catch (Exception e) {
      throw new IllegalArgumentException("invalid account store", e);
    }
  }

  synchronized Map<String, Object> current(Map<String, String> headers) {
    String value = headers.getOrDefault("cookie", "");
    for (String part : value.split(";")) {
      String[] kv = part.trim().split("=", 2);
      if (kv.length != 2 || !kv[0].equals(cookie)) continue;
      String name = tokens.get(kv[1]);
      if (name == null || System.currentTimeMillis() > expirations.getOrDefault(kv[1], 0L))
        return null;
      Map<String, Object> a = accounts.containsKey(name) ? Json.map(accounts.get(name)) : null;
      return a != null && Json.flag(a, "enabled", true) ? new LinkedHashMap<>(a) : null;
    }
    return null;
  }

  Path root(Map<String, Object> a) throws Exception {
    Path r =
        Json.flag(a, "admin", false)
            ? workspace
            : data.resolve("users").resolve(Json.text(a, "id")).resolve("workspace");
    Store.privateDir(r);
    Store.privateDir(r.resolve(".libai_agent/attachments"));
    return r;
  }

  Map<String, Object> publicAccount(Map<String, Object> a) {
    return Json.obj(
        "username",
        a.get("username"),
        "id",
        a.get("id"),
        "admin",
        a.get("admin"),
        "enabled",
        a.get("enabled"));
  }

  void require(Map<String, Object> a) {
    if (a == null) throw new App.Error(401, "authentication required");
  }

  void admin(Map<String, Object> a) {
    require(a);
    if (!Json.flag(a, "admin", false)) throw new App.Error(403, "administrator required");
  }

  synchronized List<Object> snapshot() {
    List<Object> out = new ArrayList<>();
    for (Object a : accounts.values()) out.add(new LinkedHashMap<>(Json.map(a)));
    return out;
  }

  synchronized Map<String, Object> account(String name) {
    return accounts.containsKey(name) ? new LinkedHashMap<>(Json.map(accounts.get(name))) : null;
  }

  void save() throws Exception {
    Store.write(data.resolve("auth/accounts.v1"), accounts);
  }

  void invalidate(String name) {
    tokens.entrySet().removeIf(e -> e.getValue().equals(name));
  }

  Map<String, Object> session(Map<String, Object> a, Map<String, String> out) {
    String token = Store.id();
    tokens.put(token, Json.text(a, "username"));
    expirations.put(token, System.currentTimeMillis() + 86400000);
    out.put("Set-Cookie", cookie + "=" + token + "; Path=/; HttpOnly; SameSite=Strict");
    return Json.obj(
        "ok",
        true,
        "username",
        a.get("username"),
        "admin",
        a.get("admin"),
        "authenticated",
        true,
        "initialized",
        true);
  }

  void validate(String name, String password) {
    if (!name.matches("[A-Za-z0-9_-]{1,32}") || password.length() > 1024)
      throw new App.Error(400, "invalid username or password");
    if (password.codePointCount(0, password.length()) < 6)
      throw new App.Error(400, "密码至少需要 6 个字符");
  }

  synchronized Object route(
      String op,
      String method,
      Map<String, Object> b,
      Map<String, String> headers,
      Map<String, String> out)
      throws Exception {
    Map<String, Object> a = current(headers);
    String name = Json.text(b, "username"), password = Json.text(b, "password");
    if (op.equals("status"))
      return Json.obj(
          "initialized",
          AccountStore.hasAdmin(accounts),
          "authenticated",
          a != null,
          "username",
          a == null ? "" : a.get("username"),
          "admin",
          a != null && Json.flag(a, "admin", false));
    if (op.equals("register") && method.equals("POST")) {
      if (AccountStore.hasAdmin(accounts)) throw new App.Error(409, "already initialized");
      if (accounts.containsKey(name)) throw new App.Error(409, "username already exists");
      validate(name, password);
      a =
          Json.obj(
              "username",
              name,
              "id",
              AccountStore.recoveryId(data.resolve("auth/accounts.v1"), accounts),
              "admin",
              true,
              "enabled",
              true,
              "salt",
              Store.id());
      a.put("digest", Store.password(password, Json.text(a, "salt")));
      accounts.put(name, a);
      AccountStore.rememberAdmin(data.resolve("auth/accounts.v1"), accounts);
      save();
      root(a);
      return session(a, out);
    }
    if (op.equals("login") && method.equals("POST")) {
      if (System.currentTimeMillis() - attemptedAt.getOrDefault(name, 0L) > 60000) {
        attempts.remove(name);
        attemptedAt.put(name, System.currentTimeMillis());
      }
      if (attempts.getOrDefault(name, 0) >= 10) throw new App.Error(429, "too many attempts");
      a = accounts.containsKey(name) ? Json.map(accounts.get(name)) : null;
      if (a == null
          || !Json.flag(a, "enabled", true)
          || !Store.equal(Json.text(a, "digest"), Store.password(password, Json.text(a, "salt")))) {
        attempts.put(name, attempts.getOrDefault(name, 0) + 1);
        throw new App.Error(401, "invalid credentials");
      }
      attempts.remove(name);
      return session(a, out);
    }
    require(a);
    if (op.equals("logout") && method.equals("POST")) {
      String nameNow = Json.text(a, "username");
      for (String part : headers.getOrDefault("cookie", "").split(";")) {
        String[] kv = part.trim().split("=", 2);
        if (kv.length == 2 && kv[0].equals(cookie)) tokens.remove(kv[1]);
      }
      out.put("Set-Cookie", cookie + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
      return Json.obj("ok", true);
    }
    if (op.equals("preferences")) {
      Path file = data.resolve("auth/ui-" + Json.text(a, "id") + ".v1");
      Map<String, Object> p = AccountStore.preferences(file);
      if (method.equals("POST")) {
        for (String k : new String[] {"language", "theme", "font_size"})
          if (b.containsKey(k)) {
            Object v = b.get(k);
            boolean valid = v instanceof String;
            if (k.equals("language"))
              valid =
                  valid
                      && ((String) v).matches("[a-zA-Z0-9-]{1,24}")
                      && Files.isRegularFile(html.resolve("i18n/" + v + ".json"))
                      && !v.equals("template")
                      && !v.equals("manifest");
            if (k.equals("theme"))
              valid =
                  valid
                      && Arrays.asList(
                              "default", "blue", "green", "purple", "sand", "gray", "pink", "ocean")
                          .contains(v);
            if (k.equals("font_size")) valid = valid && Arrays.asList("sm", "md", "lg").contains(v);
            if (!valid) throw new App.Error(400, "invalid preference");
            p.put(k, v);
          }
        Store.write(file, p);
      }
      return p;
    }
    if (op.equals("password") && method.equals("POST")) {
      if (!Store.equal(
          Json.text(a, "digest"),
          Store.password(Json.text(b, "current_password"), Json.text(a, "salt"))))
        throw new App.Error(403, "wrong password");
      validate(Json.text(a, "username"), password);
      a.put("salt", Store.id());
      a.put("digest", Store.password(password, Json.text(a, "salt")));
      accounts.put(Json.text(a, "username"), a);
      save();
      invalidate(Json.text(a, "username"));
      return session(a, out);
    }
    admin(a);
    if (op.equals("users") && method.equals("GET")) {
      List<Object> users = new ArrayList<>();
      for (Object v : accounts.values()) users.add(publicAccount(Json.map(v)));
      return Json.obj("users", users, "ok", true);
    }
    if (op.equals("users/create") && method.equals("POST")) {
      validate(name, password);
      if (Json.flag(b, "admin", false))
        throw new App.Error(400, "only bootstrap administrator allowed");
      if (accounts.containsKey(name)) throw new App.Error(409, "account exists");
      Map<String, Object> u =
          Json.obj(
              "username",
              name,
              "id",
              Store.id(),
              "admin",
              false,
              "enabled",
              true,
              "salt",
              Store.id());
      u.put("digest", Store.password(password, Json.text(u, "salt")));
      accounts.put(name, u);
      save();
      root(u);
      return Json.obj("ok", true);
    }
    if (op.equals("users/update") && method.equals("POST")) {
      if (!accounts.containsKey(name)) throw new App.Error(404, "account missing");
      Map<String, Object> u = Json.map(accounts.get(name));
      if (Json.flag(u, "admin", false) && !Json.flag(b, "enabled", true))
        throw new App.Error(400, "cannot disable administrator");
      if (b.containsKey("enabled")) {
        if (!(b.get("enabled") instanceof Boolean)) throw new App.Error(400, "invalid enabled");
        u.put("enabled", b.get("enabled"));
      }
      if (!password.isEmpty()) {
        validate(name, password);
        u.put("salt", Store.id());
        u.put("digest", Store.password(password, Json.text(u, "salt")));
      }
      save();
      invalidate(name);
      return Json.obj("ok", true);
    }
    throw new App.Error(404, "unknown auth route");
  }
}
