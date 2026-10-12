package io.coolder.app;

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Common JSON account records, with validated migration of original C++ records. */
final class AccountStore {
  static Map<String, Object> load(Path path) throws Exception {
    if (Files.isSymbolicLink(path)) throw new IllegalArgumentException("account file is a symlink");
    if (!Files.exists(path)) return Json.obj();
    String source = new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    boolean legacy = !source.trim().startsWith("{");
    Map<String, Object> accounts;
    if (legacy) {
      String[] words = source.trim().split("\\s+");
      if (words.length < 2
          || !words[0].equals("coolder-accounts-v1")
          || !words[1].equals("210000")
          || (words.length - 2) % 6 != 0)
        throw new IllegalArgumentException("invalid legacy account store");
      accounts = Json.obj();
      for (int i = 2; i < words.length; i += 6) {
        if (accounts.containsKey(words[i])
            || !words[i + 2].matches("[01]")
            || !words[i + 3].matches("[01]"))
          throw new IllegalArgumentException("invalid legacy account record");
        accounts.put(
            words[i],
            Json.obj(
                "username",
                words[i],
                "id",
                words[i + 1],
                "admin",
                words[i + 2].equals("1"),
                "enabled",
                words[i + 3].equals("1"),
                "salt",
                words[i + 4],
                "digest",
                words[i + 5]));
      }
    } else accounts = Json.map(Json.parse(source));
    Set<String> ids = new HashSet<>();
    int admins = 0;
    for (Map.Entry<String, Object> entry : accounts.entrySet()) {
      Map<String, Object> a = Json.map(entry.getValue());
      if (!entry.getKey().matches("[A-Za-z0-9_-]{1,32}")
          || !entry.getKey().equals(Json.text(a, "username"))
          || !(a.get("admin") instanceof Boolean)
          || !(a.get("enabled") instanceof Boolean))
        throw new IllegalArgumentException("invalid account record");
      for (String key : new String[] {"id", "salt", "digest"})
        if (!Json.text(a, key).matches("[a-f0-9]{64}"))
          throw new IllegalArgumentException("invalid account verifier");
      if (!ids.add(Json.text(a, "id")))
        throw new IllegalArgumentException("duplicate account identity");
      if (Json.flag(a, "admin", false)) {
        admins++;
        if (!Json.flag(a, "enabled", false))
          throw new IllegalArgumentException("administrator disabled");
      }
    }
    if (admins > 1)
      throw new IllegalArgumentException("account store cannot contain multiple administrators");
    migrateMembers(path.getParent().resolve("project-members.v1"));
    for (Object value : accounts.values())
      preferences(path.getParent().resolve("ui-" + Json.text(Json.map(value), "id") + ".v1"));
    if (legacy) {
      backup(path);
      Store.write(path, accounts);
    }
    rememberAdmin(path, accounts);
    return accounts;
  }

  static void migrateMembers(Path path) throws Exception {
    if (!Files.exists(path)) return;
    if (Files.isSymbolicLink(path))
      throw new IllegalArgumentException("membership file is a symlink");
    String source = new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    if (source.trim().startsWith("{")) {
      Json.parse(source);
      return;
    }
    List<Object> rows = new ArrayList<>();
    for (String line : source.split("\\r?\\n")) {
      if (line.trim().isEmpty()) continue;
      String[] words = line.trim().split("\\s+");
      if (words.length != 4) throw new IllegalArgumentException("invalid legacy membership");
      String[] fields = new String[4];
      for (int i = 0; i < 4; i++) fields[i] = (String) Json.parse(words[i]);
      if (!Arrays.asList("read", "write").contains(fields[3]))
        throw new IllegalArgumentException("invalid membership permission");
      rows.add(
          Json.obj(
              "owner",
              fields[0],
              "project",
              fields[1],
              "member",
              fields[2],
              "permission",
              fields[3]));
    }
    backup(path);
    Store.write(path, Json.obj("version", 1, "grants", rows));
  }

  static void backup(Path path) throws Exception {
    Path backup = path.resolveSibling(path.getFileName() + ".legacy");
    if (Files.isSymbolicLink(backup))
      throw new IllegalArgumentException("migration backup is a symlink");
    if (!Files.exists(backup)) {
      Files.copy(path, backup);
      Store.permissions(backup, "rw-------");
    }
  }

  static Map<String, Object> preferences(Path path) throws Exception {
    if (!Files.exists(path))
      return Json.obj("language", "zh", "theme", "default", "font_size", "md");
    if (Files.isSymbolicLink(path))
      throw new IllegalArgumentException("preferences file is a symlink");
    String source = new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    if (source.trim().startsWith("{")) return Json.map(Json.parse(source));
    String[] words = source.trim().split("\\s+");
    if (words.length != 3
        || !words[0].matches("[A-Za-z0-9-]{1,24}")
        || !Arrays.asList("default", "blue", "green", "purple", "sand", "gray", "pink", "ocean")
            .contains(words[1])
        || !Arrays.asList("sm", "md", "lg").contains(words[2]))
      throw new IllegalArgumentException("invalid legacy interface preferences");
    Map<String, Object> prefs =
        Json.obj("language", words[0], "theme", words[1], "font_size", words[2]);
    backup(path);
    Store.write(path, prefs);
    return prefs;
  }
  static boolean hasAdmin(Map<String,Object> accounts) {
    return accounts.values().stream().anyMatch(a -> Json.flag(Json.map(a),"admin",false));
  }
  static void rememberAdmin(Path path, Map<String,Object> accounts) throws Exception {
    for(Object item:accounts.values()) {
      Map<String,Object> a=Json.map(item);
      if(Json.flag(a,"admin",false)) Store.write(path.resolveSibling("administrator-identity.v1"),Json.obj("id",a.get("id")));
    }
  }
  static String recoveryId(Path path, Map<String,Object> accounts) throws Exception {
    Path marker=path.resolveSibling("administrator-identity.v1");
    String candidate="";
    if(Files.exists(marker)) candidate=Json.text(Json.map(Store.read(marker,Json.obj())),"id");
    else {
      Path backup=path.resolveSibling("accounts.v1.legacy");
      if(Files.exists(backup)) {
        if(Files.isSymbolicLink(backup)) throw new IllegalArgumentException("account backup is a symlink");
        String source=new String(Files.readAllBytes(backup),StandardCharsets.UTF_8);
        if(source.trim().startsWith("{")) {
          for(Object item:Json.map(Json.parse(source)).values()) {
            Map<String,Object> a=Json.map(item);
            if(Json.flag(a,"admin",false)) {candidate=Json.text(a,"id");break;}
          }
        } else {
          String[] fields=source.trim().split("\\s+");
          if(fields.length>=2 && fields[0].equals("coolder-accounts-v1") && fields[1].equals("210000"))
            for(int n=2;n+5<fields.length;n+=6) if(fields[n+2].equals("1")) {candidate=fields[n+1];break;}
        }
      }
    }
    if(candidate.isEmpty()) return Store.id();
    if(!candidate.matches("[0-9a-f]{64}")) throw new IllegalArgumentException("invalid administrator recovery identity");
    for(Object item:accounts.values()) if(Json.text(Json.map(item),"id").equals(candidate))
      throw new IllegalArgumentException("administrator recovery identity already in use");
    return candidate;
  }

}
