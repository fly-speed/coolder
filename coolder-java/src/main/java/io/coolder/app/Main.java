package io.coolder.app;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * Standalone Java HTTP host over libai-java's Agent and CodingService bindings; business logic is
 * Java.
 */
public final class Main {
  private static final int BODY_LIMIT = 24 * 1024 * 1024;

  private Main() {}

  private static String quote(String text) {
    StringBuilder result = new StringBuilder("\"");
    for (int i = 0; i < text.length(); ++i) {
      char c = text.charAt(i);
      if (c == '"' || c == '\\') result.append('\\').append(c);
      else if (c < 32) result.append(String.format("\\u%04x", (int) c));
      else result.append(c);
    }
    return result.append('"').toString();
  }

  private static final class Options {
    int port = 18096;
    String data = "var-java",
        workspace = "",
        html = System.getProperty("coolder.html", "html");
    String log = "coolder-java.log";
    boolean help;

    Options(String[] args) {
      for (int i = 0; i < args.length; ++i) {
        String key = args[i];
        if (key.equals("--help")) {
          help = true;
          return;
        }
        if (++i == args.length)
          throw new IllegalArgumentException("option requires a value: " + key);
        String value = args[i];
        switch (key) {
          case "--port":
            port = Integer.parseInt(value);
            break;
          case "--data":
            data = value;
            break;
          case "--workspace":
            workspace = value;
            break;
          case "--html":
            html = value;
            break;
          case "--log-file":
            log = value;
            break;
          default:
            throw new IllegalArgumentException("unknown option: " + key);
        }
      }
      if (port < 1 || port > 65535) throw new IllegalArgumentException("invalid port");
    }

    String configuration() {
      String value =
          "{\"app\":\"coolder-java\",\"cookie_name\":\"coolder_java_session\",\"data\":"
              + quote(data)
              + ",\"html\":"
              + quote(html)
              + ",\"authority\":"
              + quote("127.0.0.1:" + port)
              + ",\"log_file\":"
              + quote(log);
      String helper = System.getProperty("coolder.helper", "");
      if (!helper.isEmpty()) value += ",\"sandbox_helper\":" + quote(helper);
      if (!workspace.isEmpty()) value += ",\"workspace\":" + quote(workspace);
      return value + "}";
    }
  }

  private static byte[] readBody(InputStream stream) throws IOException {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    byte[] block = new byte[8192];
    int size;
    while ((size = stream.read(block)) != -1) {
      if (output.size() + size > BODY_LIMIT) throw new IOException("request body exceeds limit");
      output.write(block, 0, size);
    }
    return output.toByteArray();
  }

  private static final class Response implements App.Sink {
    final HttpExchange exchange;
    boolean started;

    Response(HttpExchange exchange) {
      this.exchange = exchange;
    }

    @Override
    public boolean write(int status, String fields, byte[] bytes) {
      try {
        if (!started) {
          long length = 0;
          for (String line : fields.split("\r\n")) {
            if (line.isEmpty()) continue;
            int colon = line.indexOf(':');
            String key = line.substring(0, colon), value = line.substring(colon + 1).trim();
            if (key.equalsIgnoreCase("Content-Length")) {
              length = Long.parseLong(value);
              if (length == 0) length = -1;
            } else exchange.getResponseHeaders().set(key, value);
          }
          exchange.getResponseHeaders().set("Connection", "close");
          exchange.sendResponseHeaders(status, length);
          started = true;
        }
        if (bytes.length > 0) exchange.getResponseBody().write(bytes);
        exchange.getResponseBody().flush();
        return true;
      } catch (IOException error) {
        return false;
      }
    }
  }

  private static void handle(App app, HttpExchange exchange) {
    Response response = new Response(exchange);
    try {
      String method = exchange.getRequestMethod();
      if (!method.equals("GET") && !method.equals("POST")) {
        response.write(
            405,
            "Content-Type: application/json\r\nAllow: GET, POST\r\n",
            "{\"error\":\"method not allowed\"}".getBytes(StandardCharsets.UTF_8));
        return;
      }
      StringBuilder headers = new StringBuilder();
      for (Map.Entry<String, List<String>> item : exchange.getRequestHeaders().entrySet()) {
        if (item.getValue().size() != 1) throw new IOException("duplicate request header");
        headers.append(item.getKey()).append(": ").append(item.getValue().get(0)).append("\r\n");
        if (headers.length() > 65536) throw new IOException("headers exceed limit");
      }
      String target = exchange.getRequestURI().toASCIIString();
      app.dispatch(
          method, target, headers.toString(), readBody(exchange.getRequestBody()), response);
    } catch (IOException | IllegalArgumentException error) {
      if (!response.started)
        response.write(
            400,
            "Content-Type: application/json\r\n",
            "{\"error\":\"invalid request\"}".getBytes(StandardCharsets.UTF_8));
    } catch (RuntimeException error) {
      if (!response.started)
        response.write(
            503,
            "Content-Type: application/json\r\n",
            "{\"error\":\"application unavailable\"}".getBytes(StandardCharsets.UTF_8));
    } finally {
      exchange.close();
    }
  }

  private static void run(Options options) throws Exception {
    // Binding occurs before application initialization, avoiding a service with no listener.
    HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", options.port), 64);
    ThreadPoolExecutor workers =
        new ThreadPoolExecutor(
            8,
            64,
            60,
            TimeUnit.SECONDS,
            new ArrayBlockingQueue<Runnable>(128),
            new ThreadPoolExecutor.CallerRunsPolicy());
    App app;
    try {
      app = new App(options.configuration());
    } catch (Throwable error) {
      server.stop(0);
      workers.shutdownNow();
      throw error;
    }
    CountDownLatch stopped = new CountDownLatch(1);
    Thread shutdown =
        new Thread(
            () -> {
              app.stop();
              server.stop(0);
              workers.shutdown();
              try {
                app.close();
              } catch (Exception e) {
                System.err.println(e);
              }
              stopped.countDown();
            },
            "coolder-java-shutdown");
    Runtime.getRuntime().addShutdownHook(shutdown);
    server.setExecutor(workers);
    server.createContext("/", exchange -> handle(app, exchange));
    server.start();
    System.out.println("coolder-java: http://127.0.0.1:" + options.port);
    System.out.println(
        "Workspace: "
            + Paths.get(
                    options.workspace.isEmpty() ? options.data + "/workspace" : options.workspace)
                .toAbsolutePath());
    stopped.await();
  }

  public static void main(String[] args) {
    try {
      Options options = new Options(args);
      if (options.help) {
        System.out.println(
            "coolder-java [--port 18096] [--data DIR] [--workspace DIR] [--html DIR] [--log-file FILE]");
        return;
      }
      if (options.log.isEmpty() || options.log.contains("|"))
        throw new IllegalArgumentException("invalid log file path");
      options.log = Paths.get(options.log).toAbsolutePath().toString();
      PrintStream log;
      try {
        log = new PrintStream(new FileOutputStream(options.log, true), true, "UTF-8");
      } catch (IOException error) {
        throw new IOException("cannot open log file: " + options.log, error);
      }
      System.setOut(log);
      System.setErr(log);
      System.out.println("Logging to " + options.log);
      run(options);
    } catch (Throwable error) {
      System.err.println("coolder-java: " + error.getMessage());
      System.exit(1);
    }
  }
}
