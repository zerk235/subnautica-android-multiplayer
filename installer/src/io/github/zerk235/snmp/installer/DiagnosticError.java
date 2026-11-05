package io.github.zerk235.snmp.installer;

import android.content.pm.PackageInstaller;
import java.io.*;
import java.util.Locale;
import org.json.JSONObject;

/** Stable support codes; a diagnosis never authorizes deleting a game or its data. */
final class DiagnosticError {
    final String code,title,meaning,steps;
    DiagnosticError(String code,String title,String meaning,String steps) {
        this.code=code; this.title=title; this.meaning=meaning; this.steps=steps;
    }
    static DiagnosticError of(String code,String title,String meaning,String steps) {
        return new DiagnosticError(code,title,meaning,steps);
    }
    static DiagnosticError classify(Throwable error) {
        StringBuilder chain=new StringBuilder();
        Throwable cause=error;
        for(int i=0;cause!=null && i<12;i++,cause=cause.getCause()) chain.append(cause.getClass().getSimpleName()).append(": ").append(cause.getMessage()).append('\n');
        String s=chain.toString().toLowerCase(Locale.ROOT);
        if(has(s,"enospc","no space left","свободной памяти","свободной память","миб свободной"))
            return of("STORAGE_FULL","Недостаточно места","Не хватает места для временных файлов или установки Android.","Освободите память телефона и повторите операцию. Не удаляйте игру, копии миров или данные установщика ради освобождения места.");
        if(has(s,"другом профиле","профиле android","клоне","user 11"))
            return of("GAME_OTHER_PROFILE","Игра есть в другом профиле","Копия или клон игры в другом профиле Android может мешать смене подписи.","Проверьте основной профиль и клоны приложений. Не удаляйте профиль или его миры. Отправьте диагностический ZIP разработчику для выбора безопасного способа переноса.");
        if(has(s,"shizuku","шизуку")) {
            if(has(s,"разрешите","permission denied","permission_denied"))
                return of("SHIZUKU_PERMISSION","Не разрешён доступ Shizuku","Установщик не получил разрешение на доступ к игровым файлам.","Откройте «Подключить Shizuku» и разрешите доступ для SN Multiplayer Installer. Затем повторите операцию.");
            return of("SHIZUKU_UNAVAILABLE","Shizuku недоступен","Служба не запущена, остановилась или не удалось к ней подключиться.","Запустите Shizuku, предоставьте установщику доступ и повторите операцию. При ограничении ADB откройте «Помощь с Shizuku».");
        }
        if(has(s,"eacces","eperm","permission denied","operation not permitted","доступ к adb","android не разрешил чтение"))
            return of("ACCESS_DENIED","Android ограничил доступ","Система запретила чтение или запись нужных файлов; точная причина указана в технических деталях.","Для миров и игровых логов проверьте Shizuku и его разрешение. На realme / OPPO откройте «Помощь с Shizuku». Данные приложений не очищайте.");
        if(has(s,"ключ подписи","подпись установленной игры отличается","signatures do not match","update_incompatible","другой подписью"))
            return of("SIGNATURE_CONFLICT","Не совпадает подпись","Android не может обновить установленную игру APK с другим ключом.","Сохраните игру. Сначала создайте и проверьте копию миров через Shizuku, затем следуйте шагам установщика. Не удаляйте игру вручную и не очищайте данные установщика.");
        if(has(s,"arm64","abi","no_matching_abis"))
            return of("UNSUPPORTED_ABI","Не поддерживается процессор","Эта сборка мода предназначена для Android ARM64.","Отправьте ZIP с моделью телефона и ABI. Установка на другую архитектуру сейчас не поддерживается.");
        if(has(s,"поддерживается игра","поддерживаемая игра","версия установленной игры изменилась","version_downgrade"))
            return of("GAME_VERSION","Не подходит версия игры","Версия установленной игры не соответствует поддерживаемой сборке или подготовленному APK.","Проверьте версию игры: нужна "+PatchEngine.GAME_VERSION+". После изменения игры подготовьте мод заново. Миры заранее сохраните отдельно.");
        if(has(s,"сначала установите собственную","сначала установите подготовленную","игра пока не установлена","нужна установленная игра","игра не найдена"))
            return of("GAME_NOT_FOUND","Игра не найдена в текущем профиле","Установщик не видит установленную Subnautica в этом профиле Android.","Проверьте, что игра установлена и открывается в том же профиле. Если её удалили после подготовки, установите подготовленный комплект. При наличии клона отправьте ZIP, не удаляя его.");
        if(has(s,"миры изменились","копия относится к другой","игра изменилась во время копирования"))
            return of("BACKUP_OUTDATED","Копия миров устарела","Текущие сохранения или установка игры отличаются от проверенной копии.","Сохранитесь, дождитесь окончания сохранения, выйдите в меню и создайте новую копию миров. До успешной проверки игру не удаляйте.");
        if(has(s,"сначала восстановите предыдущую","восстановите миры из проверенной"))
            return of("RESTORE_REQUIRED","Нужно вернуть сохранённые миры","После замены игры требуется восстановление уже имеющейся копии.","Не создавайте новую копию поверх прежней. Подключите Shizuku и нажмите «Восстановить миры». При ошибке отправьте ZIP разработчику.");
        if(has(s,"не удалось вернуть прежнюю папку","восстановление не подтверждено","финальная проверка миров не прошла"))
            return of("RESTORE_NOT_CONFIRMED","Восстановление не подтверждено","Проверка восстановленных миров не завершилась успешно. Пути оставшихся копий указаны в технических деталях.","Не удаляйте копии и не повторяйте замену файлов наугад. Сохраните диагностический ZIP и отправьте его разработчику до следующего восстановления.");
        if(has(s,"сначала создайте копию миров","резервная копия не найдена"))
            return of("BACKUP_MISSING","Не найдена копия миров","Нет доступной проверенной копии для этой операции.","Если игра ещё установлена, сохранитесь и создайте копию через Shizuku. Если игра удалена, импортируйте ранее сохранённый ZIP миров. Диагностический ZIP не является копией миров.");
        if(has(s,"копия повреждена","резервная копия повреждена","sha256","состав копии","копии миров не прошла","архив миров","копия миров повреждена"))
            return of("BACKUP_INVALID","Копия миров не прошла проверку","Не удалось подтвердить целостность или состав файлов копии.","Сохраните имеющийся архив. Если игра доступна, создайте свежую копию. Если игра уже удалена, отправьте отчёт разработчику; не заменяйте единственную копию пустой.");
        if(has(s,"мод обновился","пакет устарел","установленная игра изменилась","повторите подготовку"))
            return of("PREPARED_OUTDATED","Нужна новая подготовка","Подготовленные файлы относятся к прежнему моду или другой установке игры.","Нажмите «Подготовить мод», дождитесь завершения и повторите установку. Удалять игру для повторной подготовки не нужно.");
        if(has(s,"разрешите установку","unknown sources"))
            return of("INSTALL_PERMISSION","Не разрешена установка APK","Android не разрешил этому приложению устанавливать пакеты.","Разрешите «Установку из этого источника» для SN Multiplayer Installer и повторите установку.");
        if(has(s,"повреждена библиотека","неверный elf","подготовленного apk","некорректный набор split","zipexception"))
            return of("APK_INVALID","APK не прошёл проверку","Файл мода или комплект APK повреждён либо имеет неподдерживаемый состав.","Повторите подготовку. Если ошибка повторяется, сохраните диагностический ZIP и скачайте установщик из официального релиза проекта.");
        return of("OPERATION_FAILED","Операция не завершена","По одному сообщению нельзя достоверно установить причину. Полная цепочка ошибок записана в отчёт.","Сохраните диагностический ZIP, опишите последний выполненный шаг и отправьте разработчику. До выяснения причины игру и данные установщика не удаляйте.");
    }
    static boolean has(String value,String... needles) { for(String n:needles) if(value.contains(n)) return true; return false; }
    static DiagnosticError installation(int status,String detail) {
        DiagnosticError specific=classify(new IOException(detail));
        if(!specific.code.equals("OPERATION_FAILED")) return specific;
        switch(status) {
            case PackageInstaller.STATUS_FAILURE_STORAGE: return classify(new IOException("ENOSPC"));
            case PackageInstaller.STATUS_FAILURE_BLOCKED: return of("INSTALL_BLOCKED","Android заблокировал установку","Установку отклонила политика устройства, системная защита или другое приложение.","Проверьте системное сообщение и разрешение установки. Причина Android сохранена в ZIP; отправьте его, если она непонятна.");
            case PackageInstaller.STATUS_FAILURE_ABORTED: return of("INSTALL_CANCELLED","Установка отменена","Установка была отменена пользователем или прервана Android.","Откройте установщик и повторите установку готовых файлов. Повторная подготовка требуется только при ошибке проверки.");
            case PackageInstaller.STATUS_FAILURE_CONFLICT: return of("INSTALL_CONFLICT","Конфликт с установленным пакетом","Android обнаружил несовместимость с уже установленной игрой или её копией.","Проверьте подпись и профили игры по диагностическому ZIP. Не удаляйте игру без проверенной копии миров.");
            case PackageInstaller.STATUS_FAILURE_INVALID: return of("INSTALL_INVALID","Android отклонил APK","Системный установщик считает APK или набор ресурсов некорректным.","Подготовьте мод заново и повторите установку. При повторении отправьте ZIP с точным сообщением Android.");
            case PackageInstaller.STATUS_FAILURE_INCOMPATIBLE: return of("INSTALL_INCOMPATIBLE","APK несовместим с телефоном","Android сообщил о несовместимости пакета с устройством.","Отправьте ZIP с версией Android, ABI и сообщением установщика. Другие файлы игры самостоятельно не заменяйте.");
            default: return specific;
        }
    }
    JSONObject json(String action,String stage,Throwable error) throws Exception {
        StringWriter stack=new StringWriter(); error.printStackTrace(new PrintWriter(stack));
        return new JSONObject().put("code",code).put("title",title).put("meaning",meaning).put("steps",steps)
            .put("action",action).put("stage",stage).put("at",System.currentTimeMillis())
            .put("detail",Diagnostics.limit(stack.toString(),24000));
    }
    static String readable(JSONObject error) {
        if(!error.has("code")) return "Ошибок установщика пока не записано.";
        return error.optString("code")+" · "+error.optString("title")+"\nВремя: "+new java.util.Date(error.optLong("at"))+"\n\nЧто произошло:\n"+error.optString("meaning")
            +"\n\nЧто делать:\n"+error.optString("steps")+"\n\nОперация: "+error.optString("action")
            +"\nЭтап: "+error.optString("stage")+"\n\nТехнические детали:\n"+error.optString("detail");
    }
}
