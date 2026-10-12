package io.coolder.app;

import java.util.*;

/** Strict application JSON codec, independent of JNI and C++ request types. */
final class Json {
  static Map<String, Object> obj(Object... pairs) {
    Map<String, Object> m = new LinkedHashMap<>();
    for (int i = 0; i < pairs.length; i += 2) m.put((String) pairs[i], pairs[i + 1]);
    return m;
  }

  @SuppressWarnings("unchecked")
  static Map<String, Object> map(Object v) {
    if (!(v instanceof Map)) throw new IllegalArgumentException("expected JSON object");
    return (Map<String, Object>) v;
  }

  @SuppressWarnings("unchecked")
  static List<Object> list(Object v) {
    if (!(v instanceof List)) throw new IllegalArgumentException("expected JSON array");
    return (List<Object>) v;
  }

  static String text(Map<String, Object> m, String k) {
    Object v = m.get(k);
    return v instanceof String ? (String) v : "";
  }

  static long number(Map<String, Object> m, String k, long def) {
    Object v = m.get(k);
    return v instanceof Number ? ((Number) v).longValue() : def;
  }

  static boolean flag(Map<String, Object> m, String k, boolean def) {
    Object v = m.get(k);
    return v instanceof Boolean ? (Boolean) v : def;
  }

  static Object parse(String s) {
    Parser p = new Parser(s);
    Object v = p.value(0);
    p.space();
    if (p.at != s.length()) throw new IllegalArgumentException("trailing JSON");
    return v;
  }

  static String encode(Object v) {
    if (v == null) return "null";
    if (v instanceof String) {
      StringBuilder b = new StringBuilder("\"");
      for (char c : ((String) v).toCharArray()) {
        if (c == '"' || c == '\\') b.append('\\').append(c);
        else if (c < 32) b.append(String.format("\\u%04x", (int) c));
        else b.append(c);
      }
      return b.append('"').toString();
    }
    if (v instanceof Boolean || v instanceof Number) return v.toString();
    StringJoiner j =
        new StringJoiner(",", v instanceof Map ? "{" : "[", v instanceof Map ? "}" : "]");
    if (v instanceof Map)
      for (Map.Entry<String, Object> e : map(v).entrySet())
        j.add(encode(e.getKey()) + ":" + encode(e.getValue()));
    else for (Object e : list(v)) j.add(encode(e));
    return j.toString();
  }

  private static final class Parser {
    final String s;
    int at;

    Parser(String s) {
      this.s = s;
    }

    void space() {
      while (at < s.length() && " \r\n\t".indexOf(s.charAt(at)) >= 0) at++;
    }

    char take() {
      if (at == s.length()) throw new IllegalArgumentException("short JSON");
      return s.charAt(at++);
    }

    Object value(int depth) {
      if (depth > 64) throw new IllegalArgumentException("deep JSON");
      space();
      char c = take();
      if (c == '"') return string();
      if (c == '{' || c == '[') {
        boolean o = c == '{';
        Map<String, Object> m = new LinkedHashMap<>();
        List<Object> a = new ArrayList<>();
        space();
        if (at < s.length() && s.charAt(at) == (o ? '}' : ']')) {
          at++;
          return o ? m : a;
        }
        while (true) {
          space();
          String k = null;
          if (o) {
            if (take() != '"') throw new IllegalArgumentException("object key");
            k = string();
            space();
            if (take() != ':') throw new IllegalArgumentException("colon");
          }
          Object v = value(depth + 1);
          if (o) {
            if (m.containsKey(k)) throw new IllegalArgumentException("duplicate key");
            m.put(k, v);
          } else a.add(v);
          space();
          char end = take();
          if (end == (o ? '}' : ']')) return o ? m : a;
          if (end != ',') throw new IllegalArgumentException("JSON separator");
        }
      }
      at--;
      for (String word : new String[] {"true", "false", "null"})
        if (s.startsWith(word, at)) {
          at += word.length();
          return word.equals("null") ? null : word.equals("true");
        }
      int begin = at;
      while (at < s.length() && "0123456789+-.eE".indexOf(s.charAt(at)) >= 0) at++;
      String n = s.substring(begin, at);
      if (!n.matches("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?"))
        throw new IllegalArgumentException("JSON number");
      if (n.contains(".") || n.contains("e") || n.contains("E")) {
        double value = Double.parseDouble(n);
        if (!Double.isFinite(value)) throw new IllegalArgumentException("non-finite number");
        return value;
      }
      return Long.valueOf(n);
    }

    String string() {
      StringBuilder b = new StringBuilder();
      while (true) {
        char c = take();
        if (c == '"') return b.toString();
        if (c < 32) throw new IllegalArgumentException("JSON control");
        if (c == '\\') {
          c = take();
          switch (c) {
            case '"':
            case '\\':
            case '/':
              break;
            case 'b':
              c = '\b';
              break;
            case 'f':
              c = '\f';
              break;
            case 'n':
              c = '\n';
              break;
            case 'r':
              c = '\r';
              break;
            case 't':
              c = '\t';
              break;
            case 'u':
              if (at + 4 > s.length()) throw new IllegalArgumentException("unicode escape");
              c = (char) Integer.parseInt(s.substring(at, at + 4), 16);
              at += 4;
              break;
            default:
              throw new IllegalArgumentException("JSON escape");
          }
        }
        if (c == 0) throw new IllegalArgumentException("NUL");
        b.append(c);
      }
    }
  }
}
