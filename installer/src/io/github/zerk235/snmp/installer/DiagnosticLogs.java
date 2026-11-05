package io.github.zerk235.snmp.installer;

import android.os.*;
import android.system.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.regex.*;
import java.util.zip.*;
import org.json.*;

/** Read-only, fixed scope: current profile's game logs and logcat for current game PID. */
final class DiagnosticLogs {
    static final int LOG_LIMIT=6*1024*1024, LOGCAT_LIMIT=2*1024*1024;
    static final String[] PATHS={"mod_log/logs/modloader.log","Player.log","Unity/Player.log"};
    static byte[] tail(File file,int max) throws Exception {
        if(!OsConstants.S_ISREG(Os.lstat(file.getAbsolutePath()).st_mode)) throw new IOException("Log is not a regular file");
        try(RandomAccessFile input=new RandomAccessFile(file,"r")) {
            long size=input.length(); int length=(int)Math.min(size,max);
            input.seek(size-length); byte[] bytes=new byte[length]; input.readFully(bytes); return bytes;
        }
    }
    static void entry(ZipOutputStream out,String name,byte[] data) throws IOException {
        out.putNextEntry(new ZipEntry(name)); out.write(data); out.closeEntry();
    }
    static JSONObject capture(File root,OutputStream stream,boolean runtime,int profile) throws Exception {
        JSONObject meta=new JSONObject().put("capturedAt",System.currentTimeMillis()).put("serviceUid",android.os.Process.myUid());
        JSONArray files=new JSONArray(),warnings=new JSONArray();
        try(ZipOutputStream out=new ZipOutputStream(stream)) {
            String canonical=root.getCanonicalPath()+File.separator;
            for(String name:PATHS) {
                File file=new File(root,name);
                if(!file.getCanonicalPath().startsWith(canonical)) { warnings.put(name+": путь вне папки игры"); continue; }
                if(!file.exists()) { warnings.put(name+": файл отсутствует"); continue; }
                try {
                    long original=file.length(),modified=file.lastModified();
                    byte[] bytes=tail(file,LOG_LIMIT);
                    entry(out,"game/"+name,bytes);
                    files.put(new JSONObject().put("name","game/"+name).put("sourceBytes",original).put("capturedBytes",bytes.length)
                        .put("modifiedAt",modified).put("tailOnly",original>bytes.length).put("mayChangeWhileReading",true));
                    if(name.equals(PATHS[0])) entry(out,"game/log_findings.json",analyze(new String(bytes,StandardCharsets.UTF_8)).toString(2).getBytes(StandardCharsets.UTF_8));
                } catch(Exception error) { warnings.put(name+": "+error.getClass().getSimpleName()+": "+error.getMessage()); }
            }
            // A fixed short configuration value, never arbitrary mod_config files or saves.
            File role=new File(root,"mod_config/session_role.txt");
            try {
                if(role.getCanonicalPath().startsWith(canonical) && role.isFile()) {
                    String value=new String(tail(role,64),StandardCharsets.UTF_8).trim();
                    if(value.matches("(?i)([012]|auto|host|guest)")) meta.put("savedLaunchRole",value);
                }
            } catch(Exception error) { warnings.put("Роль запуска не прочитана: "+error.getMessage()); }
            if(runtime) {
                try {
                    String processes=SaveAccessService.command("/system/bin/ps","-A","-o","UID,PID,NAME");
                    JSONArray capturedPids=new JSONArray();
                    int count=0;
                    for(String process:processes.split("\n")) {
                        String[] fields=process.trim().split("\\s+");
                        if(fields.length!=3 || !fields[0].matches("[0-9]+") || !fields[1].matches("[0-9]+")
                            || !fields[2].equals(PatchEngine.GAME) || Integer.parseInt(fields[0])/100000!=profile) continue;
                        String pid=fields[1];
                        if(++count>2) break;
                        String log=SaveAccessService.command("/system/bin/logcat","-d","-b","main","-b","crash","--pid="+pid,"-v","threadtime","-t","3000");
                        byte[] bytes=log.getBytes(StandardCharsets.UTF_8);
                        if(bytes.length>LOGCAT_LIMIT) throw new IOException("Слишком большой logcat игры");
                        entry(out,"game/logcat_"+pid+".txt",bytes); capturedPids.put(Integer.parseInt(pid));
                    }
                    meta.put("logcatPids",capturedPids);
                    if(capturedPids.length()==0) warnings.put("Игра не запущена в текущем профиле: logcat по PID не собран");
                } catch(Exception error) { warnings.put("Logcat текущей игры недоступен (возможно, игра закрыта): "+error.getMessage()); }
            }
            meta.put("files",files).put("warnings",warnings).put("scope","Fixed game log files; current game PID logcat only; no SavedGames, APK or keys");
            entry(out,"game/capture.json",meta.toString(2).getBytes(StandardCharsets.UTF_8));
        }
        return meta;
    }

    /** Findings are evidence, not guesses about root cause; retain session/time boundaries. */
    static JSONObject analyze(String text) throws Exception {
        JSONArray findings=new JSONArray();
        Pattern pump=Pattern.compile("pump alive:.*?peers=(\\d+), local_valid=(\\d+), rx hello=(\\d+) state=(\\d+)");
        long lastState=-1,firstHello=0; int stalled=0; String first="",session="unknown",last="";
        boolean recorded=false;
        for(String line:text.split("\n")) {
            if(line.contains("=== log opened") || line.contains("lan pump starting")) {
                lastState=-1; stalled=0; recorded=false; session=line.trim();
            }
            Matcher m=pump.matcher(line);
            if(!m.find()) continue;
            int peers=Integer.parseInt(m.group(1)); boolean valid=m.group(2).equals("1");
            long hello=Long.parseLong(m.group(3)),state=Long.parseLong(m.group(4));
            if(peers>0 && valid && state>0 && state==lastState) {
                if(stalled++==0) { first=line.trim(); firstHello=hello; }
                last=line.trim();
                if(stalled>=4 && hello>firstHello && !recorded) {
                    findings.put(new JSONObject().put("code","NET_POSE_RX_STALLED").put("session",session)
                        .put("meaning","Пакеты положения перестали приходить, но сообщения присутствия продолжались. Причина на стороне отправки или доставки по одному этому логу не установлена.")
                        .put("firstEvidence",first).put("lastEvidence",last).put("historical",true));
                    recorded=true;
                }
            } else { stalled=0; recorded=false; }
            lastState=state;
            if(findings.length()>=32) break;
        }
        return new JSONObject().put("schema",1).put("findings",findings)
            .put("note","Анализ ограниченного фрагмента журнала. События могут относиться к прошлым запускам; сопоставьте время и описание игрока. Отсутствие находок не означает отсутствия ошибки.");
    }
}
