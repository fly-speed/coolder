package io.coolder.app;

import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.*;
import java.util.*;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;

/** Durable private app records and Java cryptographic primitives. */
final class Store {
  static final SecureRandom RANDOM = new SecureRandom();

  static String hex(byte[] b) {
    StringBuilder s = new StringBuilder();
    for (byte v : b) s.append(String.format("%02x", v & 255));
    return s.toString();
  }

  static String id() {
    byte[] b = new byte[32];
    RANDOM.nextBytes(b);
    return hex(b);
  }

  static String hash(byte[] b) {
    try {
      return hex(MessageDigest.getInstance("SHA-256").digest(b));
    } catch (Exception e) {
      throw new IllegalStateException(e);
    }
  }

  static String password(String password, String salt) {
    try {
      return hex(
          SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
              .generateSecret(
                  new PBEKeySpec(
                      password.toCharArray(), salt.getBytes(StandardCharsets.UTF_8), 210000, 256))
              .getEncoded());
    } catch (Exception e) {
      throw new IllegalStateException(e);
    }
  }

  static boolean equal(String a, String b) {
    return MessageDigest.isEqual(
        a.getBytes(StandardCharsets.UTF_8), b.getBytes(StandardCharsets.UTF_8));
  }

  static void privateDir(Path p) throws Exception {
    if (Files.isSymbolicLink(p))
      throw new IllegalArgumentException("private directory is a symlink");
    Files.createDirectories(p);
    permissions(p, "rwx------");
  }

  static void permissions(Path p, String value) throws Exception {
    if (p.getFileSystem().supportedFileAttributeViews().contains("posix"))
      Files.setPosixFilePermissions(p, PosixFilePermissions.fromString(value));
  }

  static Object read(Path p, Object fallback) throws Exception {
    if (Files.isSymbolicLink(p)) throw new IllegalArgumentException("private file is a symlink");
    return Files.exists(p)
        ? Json.parse(new String(Files.readAllBytes(p), StandardCharsets.UTF_8))
        : fallback;
  }

  static Object shared(Path canonical, Path first, Path second, Object fallback) throws Exception {
    if (Files.exists(canonical, LinkOption.NOFOLLOW_LINKS)) return read(canonical, fallback);
    Object a = read(first, null), b = read(second, null);
    if (a != null && b != null && !a.equals(b))
      throw new IllegalArgumentException(
          "conflicting legacy JSON files: " + first.getFileName() + " and " + second.getFileName());
    return a != null ? a : b != null ? b : fallback;
  }

  static void write(Path p, Object value) throws Exception {
    privateDir(p.getParent());
    if (Files.isSymbolicLink(p)) throw new IllegalArgumentException("private file is a symlink");
    Path tmp = Files.createTempFile(p.getParent(), ".save-", ".tmp");
    try {
      permissions(tmp, "rw-------");
      Files.write(tmp, Json.encode(value).getBytes(StandardCharsets.UTF_8));
      try (FileChannel c = FileChannel.open(tmp, StandardOpenOption.WRITE)) {
        c.force(true);
      }
      Files.move(tmp, p, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
      if (p.getFileSystem().supportedFileAttributeViews().contains("posix")) {
        try (FileChannel parent = FileChannel.open(p.getParent(), StandardOpenOption.READ)) {
          parent.force(true);
        }
      }
    } finally {
      Files.deleteIfExists(tmp);
    }
  }
}
