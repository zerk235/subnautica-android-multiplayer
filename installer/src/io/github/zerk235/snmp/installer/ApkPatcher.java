package io.github.zerk235.snmp.installer;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Streaming APK rewrite. No extraction of game assets and no input modification. */
final class ApkPatcher {
    static final String MAIN = "lib/arm64-v8a/libmain.so";
    static final String ORIGINAL = "lib/arm64-v8a/libmain_orig.so";
    interface Progress { void update(String stage, long done, long total) throws IOException; }

    static void requireElf(byte[] bytes) throws IOException {
        if (bytes.length < 64 || bytes[0] != 127 || bytes[1] != 'E' || bytes[2] != 'L'
                || bytes[3] != 'F' || bytes[4] != 2 || bytes[5] != 1
                || bytes[16] != 3 || bytes[17] != 0 || (bytes[18] & 255) != 183 || bytes[19] != 0)
            throw new IOException("Нужна библиотека Android ARM64 ELF");
    }

    static byte[] readSmall(InputStream stream, int max) throws IOException {
        try (InputStream in = stream; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[65536]; int n;
            while ((n = in.read(buffer)) != -1) {
                if (out.size() + (long)n > max) throw new IOException("Слишком большой служебный файл");
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new BufferedInputStream(new FileInputStream(file))) {
            byte[] buffer = new byte[1024 * 1024]; int n;
            while ((n = in.read(buffer)) != -1) digest.update(buffer, 0, n);
        }
        return hex(digest.digest());
    }

    static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) result.append(String.format(Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }

    static void rewrite(File source, File output, byte[] mod, Progress progress) throws Exception {
        if (source.getCanonicalFile().equals(output.getCanonicalFile()) || output.exists())
            throw new IOException("Выходной APK должен быть новым файлом");
        long size = source.length(), time = source.lastModified();
        if (mod != null) requireElf(mod);
        boolean created = false, success = false;
        try (ZipFile zip = new ZipFile(source)) {
            Set<String> names = new HashSet<>();
            long total = 0;
            for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements();) {
                ZipEntry e = it.nextElement();
                if (!names.add(e.getName())) throw new IOException("Повторяющаяся запись в APK");
                if (e.getSize() < 0 || e.getSize() > 4L * 1024 * 1024 * 1024)
                    throw new IOException("Неподдерживаемый размер записи APK");
                total = Math.addExact(total, e.getSize());
            }
            byte[] original = null;
            if (mod != null) {
                if (!names.contains(MAIN)) throw new IOException("В игре нет ARM64 libmain.so");
                ZipEntry entry = zip.getEntry(names.contains(ORIGINAL) ? ORIGINAL : MAIN);
                original = readSmall(zip.getInputStream(entry), 16 * 1024 * 1024);
                requireElf(original);
                if (new String(original, StandardCharsets.ISO_8859_1).contains("libmain_orig"))
                    throw new IOException("Не удалось найти исходный Unity bootstrap");
            }
            if (!output.createNewFile()) throw new IOException("Выходной файл уже существует");
            created = true;
            try (FileOutputStream file = new FileOutputStream(output);
                 CountingOutputStream count = new CountingOutputStream(new BufferedOutputStream(file, 1024 * 1024));
                 ZipOutputStream out = new ZipOutputStream(count, StandardCharsets.UTF_8)) {
                byte[] buffer = new byte[1024 * 1024]; long done = 0;
                for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements();) {
                    ZipEntry entry = it.nextElement(); String name = entry.getName();
                    if (name.startsWith("META-INF/") || (mod != null && (name.equals(MAIN) || name.equals(ORIGINAL))))
                        continue;
                    ZipEntry copy = new ZipEntry(name);
                    // Stable DOS date avoids implicit timestamp extra fields affecting alignment.
                    copy.setTime(1262304000000L);
                    copy.setMethod(entry.getMethod());
                    if (copy.getMethod() == ZipEntry.STORED) {
                        copy.setSize(entry.getSize()); copy.setCrc(entry.getCrc());
                        align(copy, count.count);
                    }
                    out.putNextEntry(copy); long copied = 0; CRC32 crc = new CRC32();
                    try (InputStream in = zip.getInputStream(entry)) {
                        int n;
                        while ((n = in.read(buffer)) != -1) {
                            copied += n; done += n;
                            if (copied > entry.getSize()) throw new IOException("Размер записи APK изменился");
                            out.write(buffer, 0, n); crc.update(buffer, 0, n);
                            progress.update("Упаковка " + source.getName(), done, total);
                        }
                    }
                    if (copied != entry.getSize() || crc.getValue() != entry.getCrc())
                        throw new IOException("Ошибка чтения данных APK");
                    out.closeEntry();
                }
                if (mod != null) {
                    stored(out, count, MAIN, mod);
                    stored(out, count, ORIGINAL, original);
                }
                out.finish(); out.flush(); file.getFD().sync();
            }
            if (source.length() != size || source.lastModified() != time)
                throw new IOException("Игра обновилась во время подготовки. Повторите попытку");
            success = true;
        } finally {
            if (created && !success && !output.delete()) output.deleteOnExit();
        }
    }

    static void stored(ZipOutputStream out, CountingOutputStream count, String name, byte[] data) throws Exception {
        ZipEntry e = new ZipEntry(name); e.setTime(1262304000000L); e.setMethod(ZipEntry.STORED);
        CRC32 crc = new CRC32(); crc.update(data); e.setCrc(crc.getValue()); e.setSize(data.length);
        align(e, count.count); out.putNextEntry(e); out.write(data); out.closeEntry();
    }

    static void align(ZipEntry entry, long offset) {
        int alignment = entry.getName().endsWith(".so") ? 16384 : 4;
        long headerEnd = offset + 30 + entry.getName().getBytes(StandardCharsets.UTF_8).length;
        int padding = (int)((alignment - (headerEnd + 4) % alignment) % alignment);
        byte[] extra = new byte[4 + padding];
        extra[0] = (byte)0xfe; extra[1] = (byte)0xca;
        extra[2] = (byte)padding; extra[3] = (byte)(padding >> 8);
        entry.setExtra(extra);
    }

    static final class CountingOutputStream extends FilterOutputStream {
        long count;
        CountingOutputStream(OutputStream output) { super(output); }
        public void write(int b) throws IOException { out.write(b); count++; }
        public void write(byte[] b, int at, int n) throws IOException { out.write(b, at, n); count += n; }
    }
}
