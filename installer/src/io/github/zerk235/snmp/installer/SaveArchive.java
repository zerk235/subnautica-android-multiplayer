package io.github.zerk235.snmp.installer;

import android.system.*;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** A bounded, checked world archive. Extraction is always into a new directory. */
final class SaveArchive {
    static final String META = "snmp-backup.json";
    static final int MAX_FILES = 20000, MAX_META = 8*1024*1024;
    static final long MAX_FILE = 512L*1024*1024, MAX_TOTAL = 4L*1024*1024*1024;

    static String name(String path) throws IOException {
        if (path.isEmpty() || path.length()>1024 || path.startsWith("/") || path.contains("\\") || path.indexOf('\0')>=0)
            throw new IOException("Некорректное имя в копии миров");
        for(String part:path.split("/",-1)) if(part.isEmpty() || part.equals(".") || part.equals(".."))
            throw new IOException("Некорректный путь в копии миров");
        return path;
    }

    static JSONObject inventory(File root) throws Exception {
        JSONObject files = new JSONObject();
        long[] total = {0};
        walk(root, root, files, total);
        return new JSONObject().put("schema",1).put("package",PatchEngine.GAME).put("version",PatchEngine.GAME_VERSION)
            .put("createdAt",System.currentTimeMillis()).put("files",files).put("bytes",total[0]);
    }
    static void walk(File root,File dir,JSONObject files,long[] total) throws Exception {
        if(root.toPath().relativize(dir.toPath()).getNameCount()>24) throw new IOException("Слишком глубокая папка сохранений");
        StructStat stat=Os.lstat(dir.getAbsolutePath());
        if(!OsConstants.S_ISDIR(stat.st_mode)) throw new IOException("Папка миров недоступна или является ссылкой");
        File[] entries=dir.listFiles();
        if(entries==null) throw new IOException("Android не разрешил чтение папки миров");
        Arrays.sort(entries,Comparator.comparing(File::getName));
        for(File file:entries) {
            StructStat info=Os.lstat(file.getAbsolutePath());
            if(OsConstants.S_ISDIR(info.st_mode)) { walk(root,file,files,total); continue; }
            if(!OsConstants.S_ISREG(info.st_mode)) throw new IOException("Неподдерживаемый файл в сохранениях");
            String path=name(root.toPath().relativize(file.toPath()).toString().replace(File.separatorChar,'/'));
            if(files.length()>=MAX_FILES || info.st_size<0 || info.st_size>MAX_FILE || (total[0]+=info.st_size)>MAX_TOTAL)
                throw new IOException("Сохранения превышают лимит резервного копирования");
            files.put(path,new JSONObject().put("size",info.st_size).put("sha256",ApkPatcher.sha256(file)));
        }
    }
    static boolean same(JSONObject left,JSONObject right) throws Exception {
        JSONObject a=left.getJSONObject("files"),b=right.getJSONObject("files");
        if(a.length()!=b.length()) return false;
        Iterator<String> keys=a.keys();
        while(keys.hasNext()) {
            String key=keys.next();
            if(!b.has(key) || a.getJSONObject(key).getLong("size")!=b.getJSONObject(key).getLong("size")
                || !a.getJSONObject(key).getString("sha256").equals(b.getJSONObject(key).getString("sha256"))) return false;
        }
        return true;
    }
    static JSONObject create(File root,OutputStream output) throws Exception {
        JSONObject before=inventory(root), files=before.getJSONObject("files");
        byte[] buffer=new byte[128*1024];
        try(ZipOutputStream zip=new ZipOutputStream(new BufferedOutputStream(output))) {
            Iterator<String> keys=files.keys();
            while(keys.hasNext()) {
                String key=keys.next(); JSONObject expected=files.getJSONObject(key);
                zip.putNextEntry(new ZipEntry("SavedGames/"+key));
                MessageDigest hash=MessageDigest.getInstance("SHA-256"); long size=0;
                try(InputStream in=new FileInputStream(new File(root,key))) {
                    int n; while((n=in.read(buffer))!=-1) {
                        size+=n; if(size>expected.getLong("size")) throw new IOException("Мир изменился во время копирования. Сохранитесь и выйдите в меню");
                        zip.write(buffer,0,n); hash.update(buffer,0,n);
                    }
                }
                if(size!=expected.getLong("size") || !ApkPatcher.hex(hash.digest()).equals(expected.getString("sha256")))
                    throw new IOException("Мир изменился во время копирования. Повторите копирование после выхода в меню");
                zip.closeEntry();
            }
            if(!same(before,inventory(root))) throw new IOException("Состав сохранений изменился во время копирования");
            byte[] meta=before.toString().getBytes(StandardCharsets.UTF_8);
            if(meta.length>MAX_META) throw new IOException("Слишком большой список файлов сохранений");
            zip.putNextEntry(new ZipEntry(META)); zip.write(meta); zip.closeEntry();
        }
        return before;
    }

    static JSONObject inspect(InputStream input,File newDirectory) throws Exception {
        if(newDirectory!=null && (!newDirectory.mkdir() || !newDirectory.isDirectory()))
            throw new IOException("Папка восстановления уже существует или недоступна");
        JSONObject actual=new JSONObject(),manifest=null;
        Set<String> seen=new HashSet<>(); byte[] buffer=new byte[128*1024]; long total=0;
        try(ZipInputStream zip=new ZipInputStream(new BufferedInputStream(input))) {
            ZipEntry entry;
            while((entry=zip.getNextEntry())!=null) {
                String path=entry.getName();
                if(!seen.add(path) || entry.isDirectory()) throw new IOException("Повторяющийся или неверный элемент архива миров");
                if(path.equals(META)) {
                    ByteArrayOutputStream meta=new ByteArrayOutputStream(); int n;
                    while((n=zip.read(buffer))!=-1) {
                        if(meta.size()+(long)n>MAX_META) throw new IOException("Слишком большой список файлов");
                        meta.write(buffer,0,n);
                    }
                    manifest=new JSONObject(new String(meta.toByteArray(),StandardCharsets.UTF_8));
                    continue;
                }
                if(!path.startsWith("SavedGames/")) throw new IOException("Лишний файл в архиве миров");
                String relative=name(path.substring("SavedGames/".length()));
                if(actual.length()>=MAX_FILES) throw new IOException("Слишком много файлов в копии миров");
                File destination=newDirectory==null?null:new File(newDirectory,relative);
                if(destination!=null) {
                    if(!destination.getCanonicalPath().startsWith(newDirectory.getCanonicalPath()+File.separator))
                        throw new IOException("Путь вышел за папку восстановления");
                    File parent=destination.getParentFile();
                    if(!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Не удалось создать папку мира");
                }
                MessageDigest hash=MessageDigest.getInstance("SHA-256"); long size=0;
                try(OutputStream out=destination==null?new OutputStream(){public void write(int b){} public void write(byte[] b,int o,int n){}}:new FileOutputStream(destination)) {
                    int n; while((n=zip.read(buffer))!=-1) {
                        size+=n; total+=n;
                        if(size>MAX_FILE || total>MAX_TOTAL) throw new IOException("Превышен размер копии миров");
                        hash.update(buffer,0,n); out.write(buffer,0,n);
                    }
                    if(out instanceof FileOutputStream) ((FileOutputStream)out).getFD().sync();
                }
                actual.put(relative,new JSONObject().put("size",size).put("sha256",ApkPatcher.hex(hash.digest())));
            }
        }
        if(manifest==null || manifest.getInt("schema")!=1 || !PatchEngine.GAME.equals(manifest.getString("package"))
            || !PatchEngine.GAME_VERSION.equals(manifest.getString("version")) || manifest.getLong("bytes")!=total
            || !same(manifest,new JSONObject().put("files",actual))) throw new IOException("Копия миров повреждена: SHA256 или состав файлов не совпал");
        return manifest;
    }

    static JSONObject restore(InputStream input,File saves) throws Exception {
        File parent=saves.getParentFile();
        if(!parent.isDirectory()) throw new IOException("Сначала один раз откройте установленную игру до меню и закройте её");
        String id=UUID.randomUUID().toString();
        File staging=new File(parent,".snmp_restore_"+id),previous=new File(parent,".snmp_previous_"+id);
        boolean movedOld=false,committed=false;
        try {
            JSONObject expected=inspect(input,staging);
            if(!same(expected,inventory(staging))) throw new IOException("Записанные файлы не прошли проверку");
            if(saves.exists()) {
                if(!OsConstants.S_ISDIR(Os.lstat(saves.getAbsolutePath()).st_mode)) throw new IOException("Неверная папка сохранений");
                if(!saves.renameTo(previous)) throw new IOException("Не удалось оставить прежние миры в резервной папке");
                movedOld=true;
            }
            if(!staging.renameTo(saves)) throw new IOException("Не удалось включить восстановленные миры");
            if(!same(expected,inventory(saves))) {
                saves.renameTo(staging);
                throw new IOException("Финальная проверка миров не прошла");
            }
            committed=true;
            return new JSONObject().put("files",expected.getJSONObject("files").length()).put("bytes",expected.getLong("bytes"))
                .put("previous",movedOld?previous.getName():JSONObject.NULL).put("verified",true);
        } finally {
            if(!committed && movedOld && !saves.exists() && !previous.renameTo(saves))
                throw new IOException("Не удалось вернуть прежнюю папку. Она сохранена: "+previous.getName());
            // Failed staging and previous worlds are retained, never deleted here.
        }
    }
}
