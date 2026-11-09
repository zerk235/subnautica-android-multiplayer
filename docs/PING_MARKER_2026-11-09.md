# Метка напарника в HUD — 9 ноября (НЕ РЕШЕНО)

> **Итог / не использовать повторно:** HUD-метка напарника через `PingInstance`/`uGUI_Pings`
> на этой Android/IL2CPP-сборке **не работает**. В исходнике (`modloader/src/player_marker.inc`)
> ветки `ping`/`nitrox`/`beacon`/`cleanup` намеренно **отключены (refuse + warning)**, чтобы их
> не включали снова. Рабочий вариант — только текстовая подсказка (`on`, направление исправлено
> на `marker_yaw = view_yaw + 180°`) и read-only `probe`. Детали неудачных попыток ниже — как
> память, чего делать не надо.

Локальная работа от 2026-11-09..10, протокол 29. Изменения не опубликованы/не вшиты в релизный lib
(на phone1 после тестов восстановлен безопасный `libmain.so` = `ff263357…`).

## Что работает (единственное)

- `on` — текст каждые ~5 с: `SNMP: напарник <id> — <dist> м, <сторона><высота>` через
  `ErrorMessage.AddMessage`. Направление: `bearing(dx,dz, local.view_yaw + 180°)` (camAnchor/body
  mismatch, как в `drive_avatar_animator`).
- `probe` — read-only инвентаризация `FindObjectsOfType(PingInstance)`.

## Что НЕ работает (не повторять)

1. **Мод создаёт `GameObject` + `AddComponent(PingInstance)` сам (тип Beacon/6 или Lifepod/1, на пустом GO или на аватаре пира):** создание/запись полей/attach логируются успешно, но ~через 1 с процесс падает в `libunity` при первом обновлении HUD (`uGUI_Pings` path). На обеих телефонах. **Не повторять live-эксперименты без статического анализа.**
2. **Nitrox-последовательность 1-в-1** (`NitroxClient/.../PlayerPing.cs`: `AddComponent<PingInstance>`, `Signal(7)`, `origin`, `minDist=5`, `_label`, `displayPingInManager=false`, `visible=true`, явный `Initialize()`): `Initialize` вернулся (регистрация прошла), но на следующем кадре падение в `libil2cpp` → `uGUI_Pings.UpdatePings` (`fault addr 0x63e`, **`libmain` в стеке отсутствует** — падает покадровый код игры, рисующий пинг). На Android/IL2CPP это не лечится порядком полей.
3. **Спавн префаба Beacon (TechType 508) через `find_prefab_source`/`Instantiate`:** префаб спавнится, `PingInstance` регистрируется (виден в ПДА, `PingManager` содержит его, `visible/range/label` выставляются, `SetVisible`/`NotifyVisible`/`NotifyRename` вызываются), но **иконка/метка в HUD с дистанцией не появилась** в проверках; плюс получение префаба тайминг-зависимо (`find_prefab_source(508)` может долго/нестабильно возвращать `null` при `world_ops=1`). Как HUD-маркер напарника — нерабочий результат.
4. **`cleanup` по label-эвристике** — небезопасен (риск задеть не те объекты/нагрузка); рантайм-маяки не сохраняются между сессиями. Убран.

## История попыток (справочно, не инструкция к применению)

Текстовая подсказка (`ErrorMessage.AddMessage`) видна, но ищется не там — игрок ожидает маркер в мире. Попытка использовать родную систему пингов (`PingInstance` на объекте + регистрация через `OnEnable`/`PingManager.Register`, проекция `uGUI_Pings`, как у техники/маяков): `pingType Beacon(6)`, `origin=transform`, `range=500`, `minDist=0`, `visible`, `_id`, подпись «Напарник», позиция из `PeerState`. `PingType` «игрок» в сборке отсутствует. Детали по каждой неудачной ветке — выше и в секциях ниже.

## Порядок создания объекта

Создание сделано в три шага намеренно, чтобы не ловить null в `Initialize()`:

1. `new GameObject("SNMP_PeerMarker")`;
2. `SetActive(false)` — `AddComponent` на неактивном объекте не запускает
   `OnEnable`, значит `PingInstance.Initialize()` ещё не читает поля;
3. заполнение полей, затем `SetActive(true)`, что запускает `OnEnable` →
   `Initialize()` → `PingManager.Register`.

## Включение

Создание маркера не автоматическое, а по файлу-запросу, как у опытов с рыбой:

```
/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_log/ping_marker.request
```

Содержимое — `on` (текстовые подсказки) или `probe` (инвентаризация). `ping`/`nitrox`/`beacon`/`cleanup`
**отключены** (refuse) — см. вердикт. Файл удаляется после чтения.

Тексты `SNMP: напарник …` остаются как резерв: они проверяют, что сессия жива,
даже если система пингов недоступна.

## Лог

```
ping marker: api ready register=… set_label=… ctor=…
ping marker: armed by request file
ping marker: created go=… instance=… type=6 range=500
player marker: peer=… dist=… side=… ping=1 updates=…
ping marker: dropped reason=… updates=…
```

## Состояние проверки

- код собран, оба телефона обновлены (игра и установщик), протокол не менялся;
- текстовые подсказки подтверждены вживую до этой сборки;
- создание собственного `PingInstance` **уронило игру на обоих телефонах**
  (SIGSEGV в `libunity`, кадры `#00-#02`, через ~1 с после создания объекта).
  Дамп: `unity` падает на объекте, созданном модом, хотя `new GameObject` +
  `AddComponent` + `SetActive(true)` и логирование `created` прошли целиком.
  Причина не установлена; вероятнее всего, объект не имеет сцены/владельца,
  которых ждёт `uGUI_Pings` при первом обновлении.

Текущее состояние (в исходнике, намеренно): HUD-создание **отключено**.

| Содержимое `ping_marker.request` | Поведение |
| --- | --- |
| `on` | только текстовые подсказки с направлением (безопасно) |
| `probe` | однократный read-only список всех `PingInstance` в мире |
| `ping` / `nitrox` / `beacon` / `cleanup` | **ОТКАЗ + warning** (пробовали, не работает на Android; см. вердикт выше) — не использовать |

На phone1 после тестов восстановлен безопасный lib `ff263357…` (см. выше).

### Вторая попытка: пинг на аватаре

Создание объекта исключили полностью: `PingInstance` добавляется в **существующий клон
аватара напарника** — настоящий объект сцены, который игра сама двигает и уничтожает.
`AddComponent` в этом моде уже применяется к аватарам (SkyApplier) и не падал.
Тип пинга взяли `Lifepod(1)` — такой пинг в загруженном мире уже есть, значит иконка
для него заведомо существует (версию «Beacon вне таблицы иконок» отбросили).

Результат: `ping marker: attached to avatar … type=1` и через ~1 с снова SIGSEGV в
`libunity`, на обоих телефонах.

Вывод: падает **не создание и не тип**, а сам путь регистрации/отрисовки пинга
(`PingManager.Register` → `uGUI_Pings`). Дальше без статического разбора
`PingInstance.Initialize`/`uGUI_Pings` двигаться нельзя: каждая живая попытка
стоит обоим игрокам падения. Поэтому запрос `ping` теперь отклоняется и пишет
предупреждение в лог.

### Направления

Первая версия подсказок показывала стороны зеркально: напарник сзади читался как
«впереди», лево и право менялись местами. Причина — `view_yaw` берётся с
`Player.camAnchor`, а его yaw идёт в противоположной конвенции относительно
`atan2(dx, dz)`, то есть ошибка ровно 180°. Это та же нестыковка, из-за которой в
`drive_avatar_animator` понадобился `+180`. Исправлено: в `bearing()` передаётся
`view_yaw + 180`, а в лог добавлены `world_deg`, `view_yaw`, `marker_yaw` для
сверки.


## Разбор на ПК (без запуска игры)

Взяты адреса из `dump.cs`, дизассемблировано `libil2cpp.so` (`llvm-objdump` из NDK).

`PingInstance.Initialize` (`0x29D4CC0`):
- если `_id` (0x58) пустой — генерируется `Guid` и пишется в `_id`;
- `currentVersion` (0x48) := 2, `initialized` (0x60) := 1;
- вызывается `PingManager.Register(this)` (`0x29D4A60`);
- `AddNotificationInternal` (`0x29D4FE4`) — только если `visitable`(0x3C) или `notify`(0x70).

`PingInstance.OnEnable` (`0x29D49F8`) регистрирует только при `initialized != 0`, то есть
всю работу делает `Start` → `Initialize`.

`uGUI_Pings.OnAdd` (`0x2C97BF8`) по порядку:
1. берёт объект из `poolPings` (0x50), вызывает `uGUI_Ping.Initialize`;
2. `SetVisible(instance.visible)` — поле 0x4C;
3. `SetColor(PingManager.colorOptions[instance.colorIndex])` — `colorIndex` 0x50,
   массив `colorOptions` лежит в статических полях `PingManager` по смещению 0x18;
4. `SetIcon(sprite для instance.pingType)` — тип 0x20;
5. `SetLabel(instance._label)` — 0x68;
6. `pings[instance.Id] = ping`.

Существенно: все отказы внутри `OnAdd` — это **вызовы raise-хелперов il2cpp**
(`0x24FD698` null-reference, `0x24FD6A0` index-out-of-range), то есть managed-исключение,
а не тихий выход. Исключение, поднятое когда внизу стека наш нативный кадр (мы вызвали
`AddComponent`/`Register`), некуда разворачивать — процесс умирает. Это согласуется с
наблюдением: падение происходит не на создании, а на первом обновлении пингов.

Проверенные значения у нашего пинга были корректны (`colorIndex=0`, `visible=1`,
`pingType=1`), поэтому точное место отказа ещё не локализовано; дальше нужен либо
Ghidra с восстановлением `PingInstance.Start`, либо отказ от ручной регистрации.

## Проверка по Nitrox (эталон)

Найдено в Nitrox: `NitroxClient/MonoBehaviours/Gui/InGame/PlayerPing.cs`. Они делают ровно
то, что нужно:

```csharp
var go = new GameObject(name);
go.transform.position = location;
var signal = go.AddComponent<PingInstance>();
signal.pingType = PingType.Signal;      // 7
signal.origin = go.transform;
signal.minDist = 5f;
signal._label = labelText;
signal.displayPingInManager = false;
signal.visible = true;
signal.Initialize();                    // явно, не через Start
```

Последовательность перенесена в мод дословно (`ping_marker_create_nitrox`), включая
`Signal`, `minDist = 5`, неустановленный `range`, пустой `_id` и явный вызов `Initialize`.

Результат на телефоне (изолированный тест, без второго игрока):

```
ping marker: armed with the Nitrox sequence (Signal, minDist 5, explicit Initialize)
ping marker: API ready register=… set_label=… ctor=…
ping marker: nitrox sequence prepare go=… instance=… type=Signal minDist=5
ping marker: nitrox Initialize returned
```

`Initialize` отработал и вернулся без исключения — то есть регистрация в `PingManager`
и `uGUI_Pings.OnAdd` прошли. Процесс умер на следующем кадре.

Бэктрейс этого падения:

```
#00-#02  libunity.so
#03-#08  libil2cpp.so        (uGUI_Pings.UpdatePings и его вызовы)
#09-#17  libunity.so        (PlayerLoop / сообщения Unity)
```

**libmain в стеке нет.** Значит падение не в нашем вызове и не в создании объекта, а в
собственном покадровом коде игры, который пытается нарисовать наш пинг: `fault addr
0x63e`, то есть чтение нативного поля по мусорному указателю.

Вывод: на Android/IL2CPP-сборке пинг, созданный в рантайме, регистрируется, но не
переживает первого обновления HUD. Это не лечится порядком полей — что-то в мобильном
рендере пингов ожидает данные, которых у объекта «из воздуха» нет.


`tools/hotswap.py` (проверен): сборка → `su cp` свежего `libsnmod.so` в
`/data/app/.../lib/arm64/libmain.so` → перезапуск игры → хвост лога. Переустановка и
установщик не нужны, второй телефон для проверки мода тоже. Цикл: ~24–41 с сборка,
~3 с подмена, дальше обычная загрузка мира.

Запрос на включение мод съедает (файл удаляется после чтения), поэтому перед каждым
прогоном `ping_marker.request` нужно класть заново.



## Ограничения

- несколько пиров: сейчас маркер ведётся за первым живым пиром;
- тип иконки один и тот же для всех;
- переживание выгрузки сцены: при потере пира маркер гасится, при возврате
  создаётся заново;
- пинг виден и в ПДА-вкладке пингов, если снять `displayPingInManager = false`.
