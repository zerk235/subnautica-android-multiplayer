package io.github.zerk235.snmp.installer;

import java.io.IOException;
import java.util.*;
import java.util.regex.*;
import org.json.*;

/** Parse only explicit PackageManager installation records, never permission sections. */
final class GameProfiles {
    final SortedSet<Integer> installed = new TreeSet<>();
    final Set<Integer> known = new HashSet<>();
    GameProfiles(String dump) throws IOException {
        Matcher rows=Pattern.compile("(?m)^\\s*User (\\d+):([^\\r\\n]*)").matcher(dump);
        while(rows.find()) {
            Matcher flag=Pattern.compile("\\binstalled=(true|false)\\b").matcher(rows.group(2));
            if(!flag.find()) continue;
            int id;
            try { id=Integer.parseInt(rows.group(1)); }
            catch(NumberFormatException error) { throw new IOException("Некорректный профиль Android",error); }
            if(!known.add(id)) throw new IOException("Неоднозначные сведения о профиле Android "+id);
            if("true".equals(flag.group(1))) installed.add(id);
        }
        if(known.isEmpty()) throw new IOException("Не удалось прочитать профили игры. Удаление заблокировано");
    }
    void requireOnly(int user) throws IOException {
        if(!installed.contains(user)) throw new IOException("Не удалось подтвердить текущий профиль игры. Удаление заблокировано");
        for(int id:installed) if(id!=user) throw new IOException("Subnautica также установлена в профиле Android "+id
            +" (возможно, клон). Новая подпись будет конфликтовать с ним. Удаление текущей игры заблокировано; другие профили и их миры не меняем.");
    }
    void requireRecovery(int user) throws IOException {
        if(installed.contains(user)) throw new IOException("Игра уже установлена в текущем профиле. Восстановление регистрации не требуется");
        if(installed.isEmpty()) throw new IOException("Не найдена установленная копия игры в другом профиле Android");
    }
    JSONObject json(int user) throws JSONException {
        return new JSONObject().put("user",user).put("currentInstalled",installed.contains(user))
            .put("installedUsers",new JSONArray(installed));
    }
}
