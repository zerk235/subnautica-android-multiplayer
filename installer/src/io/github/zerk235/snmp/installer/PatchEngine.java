package io.github.zerk235.snmp.installer;

import android.content.Context;
import android.content.pm.*;
import android.os.Build;
import android.os.StatFs;
import android.net.Uri;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;

final class PatchEngine {
    static final String GAME = "com.unknownworlds.subnautica";
    static final String GAME_VERSION = "1.22.83315";
    static final String ASSETS_SPLIT = "base_assets";
    static final int PLAN_SCHEMA = 2;
    static final long MIB = 1024L * 1024;
    final Context context;
    final File prepared, staging;
    PatchEngine(Context context) {
        this.context = context.getApplicationContext();
        prepared = new File(context.getFilesDir(), "prepared");
        staging = new File(context.getFilesDir(), "preparing");
    }

    JSONObject payload() throws Exception {
        return new JSONObject(new String(ApkPatcher.readSmall(context.getAssets().open("mod.json"), 65536), StandardCharsets.UTF_8));
    }

    PackageInfo installed() throws Exception {
        try {
            PackageInfo info=context.getPackageManager().getPackageInfo(GAME, PackageManager.GET_SIGNING_CERTIFICATES);
            return installedHere(info,android.os.Process.myUid()/100000)?info:null;
        }
        catch (PackageManager.NameNotFoundException e) { return null; }
    }
    static boolean installedHere(PackageInfo info,int user) {
        return info!=null && info.applicationInfo!=null
            && (info.applicationInfo.flags & ApplicationInfo.FLAG_INSTALLED)!=0
            && info.applicationInfo.uid/100000==user;
    }
    PackageInfo retained() throws Exception {
        try { return context.getPackageManager().getPackageInfo(GAME,PackageManager.GET_SIGNING_CERTIFICATES|PackageManager.MATCH_UNINSTALLED_PACKAGES); }
        catch(PackageManager.NameNotFoundException error) { return null; }
    }

    static String certificate(PackageInfo info) throws Exception {
        if (info == null || info.signingInfo == null || info.signingInfo.hasMultipleSigners())
            throw new IOException("Не удалось определить единственную подпись игры");
        Signature[] signatures = info.signingInfo.getApkContentsSigners();
        if (signatures == null || signatures.length != 1) throw new IOException("Неподдерживаемая подпись игры");
        return ApkPatcher.hex(MessageDigest.getInstance("SHA-256").digest(signatures[0].toByteArray()));
    }

    static boolean hasAssets(PackageInfo game) {
        return game != null && game.splitNames != null && Arrays.asList(game.splitNames).contains(ASSETS_SPLIT);
    }

    void requireAssetsRecovery(PackageInfo game, LocalSigner signer) throws Exception {
        if (game == null || !GAME_VERSION.equals(game.versionName) || hasAssets(game))
            throw new IOException("Восстановление пакета ресурсов для этой игры не требуется или не поддерживается");
        if (!certificate(game).equals(signer.fingerprint()))
            throw new IOException("Ключ установщика не совпадает с подписью игры. Восстановление ресурсов заблокировано");
    }

    File prepareAssetsRecovery(Uri source, File input, File signed, ApkPatcher.Progress progress) throws Exception {
        if (source == null || !"content".equals(source.getScheme())) throw new IOException("Выберите локальный APK ресурсов");
        LocalSigner signer = new LocalSigner();
        PackageInfo game = installed();
        requireAssetsRecovery(game, signer);
        if (new StatFs(context.getFilesDir().getPath()).getAvailableBytes() < 7L * 1024 * MIB)
            throw new IOException("Для копирования, подписи и установки ресурсов нужно около 7 ГиБ свободной памяти");
        try (InputStream in = context.getContentResolver().openInputStream(source);
             FileOutputStream out = new FileOutputStream(input)) {
            if (in == null) throw new IOException("Не удалось открыть пакет ресурсов");
            byte[] buffer = new byte[1024 * 1024]; long total = 0; int n;
            while ((n = in.read(buffer)) != -1) {
                total += n;
                if (total > 2300 * MIB) throw new IOException("Пакет ресурсов превышает допустимый размер");
                out.write(buffer, 0, n);
            }
        }
        ApkIdentity assets = ApkIdentity.read(input);
        if (!GAME.equals(assets.packageName) || !ASSETS_SPLIT.equals(assets.split) || assets.versionCode != game.getLongVersionCode())
            throw new IOException("Выберите split_base_assets.apk от установленной версии Subnautica");
        progress.update("Подпись пакета ресурсов — подождите", 0, 0);
        signer.sign(input, signed);
        PackageInfo current = installed();
        requireAssetsRecovery(current, signer);
        if (current.getLongVersionCode() != assets.versionCode || !current.applicationInfo.sourceDir.equals(game.applicationInfo.sourceDir))
            throw new IOException("Установленная игра изменилась. Повторите восстановление ресурсов");
        File base = new File(current.applicationInfo.sourceDir);
        signer.verify(base);
        return base;
    }

    void prepare(ApkPatcher.Progress progress) throws Exception {
        if (!Arrays.asList(Build.SUPPORTED_ABIS).contains("arm64-v8a")) throw new IOException("Нужен телефон Android ARM64");
        PackageInfo game = installed();
        if (game == null) throw new IOException("Сначала установите собственную копию Subnautica " + GAME_VERSION);
        if (!GAME_VERSION.equals(game.versionName))
            throw new IOException("Поддерживается игра " + GAME_VERSION + "; установлена " + game.versionName);
        if (!hasAssets(game)) throw new IOException("В игре отсутствует пакет ресурсов. Нажмите «Восстановить пакет ресурсов»");
        String sourceCert = certificate(game);
        LocalSigner signer = new LocalSigner();
        boolean full = !sourceCert.equals(signer.fingerprint());
        // Detect a clone conflict before spending time preparing a replacement or offering deletion.
        if(full && SaveBridge.readiness()==null) try(SaveBridge bridge=new SaveBridge(context)) {
            bridge.call(SaveAccessService.CHECK_PROFILES,null,false);
        }
        JSONObject modInfo = payload();
        byte[] mod = ApkPatcher.readSmall(context.getAssets().open("libsnmod.so"), 16 * 1024 * 1024);
        String modSha = ApkPatcher.hex(MessageDigest.getInstance("SHA-256").digest(mod));
        if (!modSha.equals(modInfo.getString("sha256"))) throw new IOException("Повреждена библиотека мода в установщике");
        ApkPatcher.requireElf(mod);
        List<File> sources = new ArrayList<>(); sources.add(new File(game.applicationInfo.sourceDir));
        if (game.applicationInfo.splitSourceDirs != null)
            for (String path : game.applicationInfo.splitSourceDirs) sources.add(new File(path));
        if (sources.size() > 32) throw new IOException("Слишком много APK-пакетов игры");
        long total = 0, largest = 0;
        for (File file : sources) {
            if (!file.isFile() || !file.canRead()) throw new IOException("Android не разрешил чтение установленного APK");
            total = Math.addExact(total, file.length()); largest = Math.max(largest, file.length());
        }
        if (new StatFs(context.getFilesDir().getPath()).getAvailableBytes() < total + largest + 512 * MIB)
            throw new IOException("Недостаточно свободной памяти. Нужно ещё примерно " + ((total + largest + 512*MIB)/MIB) + " МиБ");
        removeOwnTree(staging); if (!staging.mkdir()) throw new IOException("Не удалось создать рабочую папку");
        boolean success = false;
        try {
            JSONArray apks = new JSONArray();
            Set<String> splitNames = new HashSet<>();
            for (int i = 0; i < sources.size(); i++) {
                File source = sources.get(i);
                String name = i == 0 ? "base.apk" : "split_" + i + ".apk";
                File unsigned = new File(staging, "unsigned.apk");
                File signed = new File(staging, name);
                progress.update(i == 0 ? "Добавление мода" : "Подготовка ресурсов " + i + "/" + (sources.size()-1), 0, source.length());
                if (i > 0 && !full) {
                    copySplit(source, signed, progress);
                    signer.verify(signed);
                } else {
                    ApkPatcher.rewrite(source, unsigned, i == 0 ? mod : null, progress);
                    progress.update("Подпись " + (i+1) + "/" + sources.size() + " — подождите", 0, 0);
                    signer.sign(unsigned, signed);
                    if (!unsigned.delete()) throw new IOException("Не удалось удалить временный APK");
                }
                ApkIdentity archive = ApkIdentity.read(signed);
                if (!GAME.equals(archive.packageName) || archive.versionCode != game.getLongVersionCode()
                        || (i == 0 && !GAME_VERSION.equals(archive.versionName))) throw new IOException("Метаданные подготовленного APK не совпадают с игрой");
                String split = archive.split;
                if ((i == 0 && split != null) || (i != 0 && (split == null || !splitNames.add(split))))
                    throw new IOException("Некорректный набор split APK");
                apks.put(new JSONObject().put("name", name).put("size", signed.length()).put("sha256", ApkPatcher.sha256(signed)));
            }
            if (!splitNames.contains(ASSETS_SPLIT)) throw new IOException("В подготовленном пакете нет ресурсов Subnautica");
            PackageInfo current = installed();
            if (current == null || current.getLongVersionCode() != game.getLongVersionCode()
                    || !certificate(current).equals(sourceCert) || !Objects.equals(current.applicationInfo.sourceDir, game.applicationInfo.sourceDir))
                throw new IOException("Установленная игра изменилась. Повторите подготовку");
            JSONObject plan = new JSONObject().put("schema", PLAN_SCHEMA).put("package", GAME).put("version", GAME_VERSION)
                .put("versionCode", game.getLongVersionCode()).put("full", full).put("buildId", modInfo.getString("buildId"))
                .put("cert", signer.fingerprint()).put("apks", apks);
            write(new File(staging, "plan.json"), plan.toString(2).getBytes(StandardCharsets.UTF_8));
            File previous = new File(context.getFilesDir(), "prepared-previous");
            removeOwnTree(previous);
            if (prepared.exists() && !prepared.renameTo(previous)) throw new IOException("Не удалось сохранить предыдущий пакет");
            if (!staging.renameTo(prepared)) {
                if (previous.exists()) previous.renameTo(prepared);
                throw new IOException("Не удалось завершить подготовку");
            }
            success = true;
            removeOwnTree(previous);
        } finally { if (!success) removeOwnTree(staging); }
    }

    private static void copySplit(File source, File target, ApkPatcher.Progress progress) throws IOException {
        try (InputStream in = new FileInputStream(source); FileOutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1024 * 1024];
            long done = 0, size = source.length();
            for (int n; (n = in.read(buffer)) != -1;) {
                out.write(buffer, 0, n);
                done += n;
                progress.update("Копирование ресурсов игры", done, size);
            }
            out.getFD().sync();
        }
    }

    JSONObject plan() throws Exception {
        File file = new File(prepared, "plan.json");
        if (!file.isFile()) return null;
        JSONObject plan = new JSONObject(new String(ApkPatcher.readSmall(new FileInputStream(file), 65536), StandardCharsets.UTF_8));
        if (plan.getInt("schema") != PLAN_SCHEMA || !GAME.equals(plan.getString("package")) || !GAME_VERSION.equals(plan.getString("version")))
            throw new IOException("Пакет устарел. Подготовьте его заново");
        if (!payload().getString("buildId").equals(plan.getString("buildId")))
            throw new IOException("Мод обновился. Нажмите «Подготовить мод»");
        return plan;
    }

    String installBlocker(JSONObject plan) throws Exception {
        if (plan == null) return "Сначала подготовьте мод";
        PackageInfo current = installed();
        if (current == null) {
            PackageInfo other=retained();
            if(other!=null && !certificate(other).equals(plan.getString("cert")))
                return "Subnautica отсутствует в текущем профиле, но её пакет остался в другом профиле Android или клоне с другой подписью. Установка новой подписи заблокирована. Не удаляйте другие профили и их миры: сначала верните существующую игру в текущий профиль и восстановите копию миров.";
            return plan.getBoolean("full") ? null : "Нужна установленная игра: этот пакет содержит только обновление";
        }
        if (!hasAssets(current)) return "В игре отсутствует пакет ресурсов. Нажмите «Восстановить пакет ресурсов»";
        if (!certificate(current).equals(plan.getString("cert")))
            return "Подпись установленной игры отличается. Для замены подключите Shizuku, создайте копию миров и нажмите «Удалить старую игру». Перед системным запросом установщик проверит актуальность копии и APK. Затем установите мод и восстановите миры. Не удаляйте игру вручную без проверенной копии.";
        if (current.getLongVersionCode() != plan.getLong("versionCode") || !GAME_VERSION.equals(current.versionName))
            return "Версия установленной игры изменилась. Подготовьте мод заново";
        return null;
    }

    String removalBlocker(JSONObject plan) throws Exception {
        PackageInfo game=installed();
        if(game==null || plan==null || !plan.getBoolean("full") || !GAME_VERSION.equals(game.versionName)
            || game.getLongVersionCode()!=plan.getLong("versionCode") || certificate(game).equals(plan.getString("cert")))
            return "Удаление для этой установки не требуется. Подготовьте мод заново при изменении игры.";
        if(new SaveManager(context).needsRestore()) return "Сначала восстановите предыдущую копию миров";
        return null;
    }

    String removalIdentity() throws Exception {
        PackageInfo info=installed();
        if(info==null) throw new IOException("Игра не найдена в текущем профиле");
        return info.applicationInfo.uid+":"+info.getLongVersionCode()+":"+certificate(info)+":"+info.applicationInfo.sourceDir;
    }

    void verifyInstalled(JSONObject plan) throws Exception {
        PackageInfo info=installed();
        if(info==null || context.getPackageManager().getLaunchIntentForPackage(GAME)==null)
            throw new IOException("Игра не найдена в текущем профиле после установки. Проверьте восстановление игры из другого профиля Android");
        if(info.getLongVersionCode()!=plan.getLong("versionCode") || !certificate(info).equals(plan.getString("cert")))
            throw new IOException("Подпись или версия игры после установки не совпала с подготовленным пакетом");
        verifyAssetsInstalled(info);
        try(java.util.zip.ZipFile apk=new java.util.zip.ZipFile(info.applicationInfo.sourceDir)) {
            java.util.zip.ZipEntry entry=apk.getEntry(ApkPatcher.MAIN);
            if(entry==null) throw new IOException("Установленный APK не содержит библиотеку мода");
            byte[] library=ApkPatcher.readSmall(apk.getInputStream(entry),16*1024*1024);
            if(!ApkPatcher.hex(MessageDigest.getInstance("SHA-256").digest(library)).equals(payload().getString("sha256")))
                throw new IOException("SHA256 установленной библиотеки мода не совпал с подготовленной сборкой");
        }
    }

    void verifyAssetsInstalled(PackageInfo info) throws IOException {
        if (!hasAssets(info)) throw new IOException("Android не установил пакет ресурсов split_base_assets.apk");
    }

    List<File> verifyPlan(JSONObject plan, ApkPatcher.Progress progress) throws Exception {
        LocalSigner signer = new LocalSigner();
        if (!signer.fingerprint().equals(plan.getString("cert"))) throw new IOException("Ключ подписи изменился. Подготовьте мод заново");
        JSONArray entries = plan.getJSONArray("apks");
        if (entries.length() < 2 || entries.length() > 32)
            throw new IOException("Некорректный список APK");
        List<File> files = new ArrayList<>(); Set<String> names = new HashSet<>(), splits = new HashSet<>();
        for (int i = 0; i < entries.length(); i++) {
            JSONObject entry = entries.getJSONObject(i); String name = entry.getString("name");
            if (!name.matches("base\\.apk|split_[0-9]+\\.apk") || !names.add(name) || (i == 0 && !name.equals("base.apk")))
                throw new IOException("Некорректное имя APK");
            File file = new File(prepared, name);
            progress.update("Проверка APK " + (i+1) + "/" + entries.length(), 0, 0);
            if (file.length() != entry.getLong("size") || !ApkPatcher.sha256(file).equals(entry.getString("sha256")))
                throw new IOException("Подготовленный APK повреждён. Подготовьте мод заново");
            signer.verify(file); ApkIdentity info = ApkIdentity.read(file);
            if (!GAME.equals(info.packageName) || info.versionCode != plan.getLong("versionCode")) throw new IOException("Неверный APK в подготовленном пакете");
            if (i == 0 ? info.split != null : info.split == null || !splits.add(info.split))
                throw new IOException("Некорректный набор split APK");
            files.add(file);
        }
        if (!splits.contains(ASSETS_SPLIT)) throw new IOException("В подготовленном пакете нет ресурсов Subnautica");
        return files;
    }

    void removeOwnTree(File file) throws IOException {
        String parent = context.getFilesDir().getCanonicalPath() + File.separator;
        if (!file.getCanonicalPath().startsWith(parent)) throw new IOException("Некорректная рабочая папка");
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) removeOwnTree(child);
        if (!file.delete()) throw new IOException("Не удалось удалить временный файл " + file.getName());
    }

    static void write(File file, byte[] bytes) throws IOException {
        try (FileOutputStream out = new FileOutputStream(file)) { out.write(bytes); out.getFD().sync(); }
    }
}
