# SpawnGuard 2.0

Плагин для **ARK: Survival Ascended** (AsaApi). Запрещает строительство рядом с:

* точками спавна игроков (все точки спавна карты, находятся автоматически),
* supply drops — зоны приземления (объёмы `ASupplyCrateSpawningVolume`), уже
  упавшие ящики и встроенная движковая проверка `IsPointNearSupplyCrateSpawn`,
* заметками исследователей — всё, что наследуется от `ExplorerChest_Base_C`:
  записки, досье, глитчи Extinction, руны Fjordur, HLN-A и т.д.,
* кастомными зонами, которые админ ставит командами `SG.AddPoint` / `SG.RemovePoint`.

---

## 1. Что было исправлено по сравнению с первой версией

### 1.1 Критично: блокировка не работала вообще (возврат `0`)

`APrimalStructure.IsAllowedToBuild(...)` возвращает `int`, и это **«причина»,
по которой строить нельзя**, а не «разрешение»:

* `0` — причин нет, строить можно (`0` = «no reason to block»);
* не `0` — причина есть.

Доказательства прямо в SDK: `int BPIsAllowedToBuild(const FPlacementData*
OutPlacementData, int CurrentAllowedReason)` — параметр называется
«текущая причина»; `void GetCantBuildReasonString(int ReasonVal, FString*
reasonString)` — движок по этому числу строит текст «почему нельзя строить».

Старый код при срабатывании зоны возвращал `0`, т.е. сообщал движку
«всё в порядке» → **запрета не было, только сообщение игроку**. Теперь
возвращается `General.BlockReturnValue` (по умолчанию `1`).
Значение `0` в конфиге = режим «только уведомлять, не запрещать».

Проверить семантику на своём сервере можно по одной строке в `ArkApi.log`:

```
SpawnGuard diagnostics: IsAllowedToBuild returned: [0]=57 [4]=3
(0 = 'no reason to block' = allowed, non-zero = 'reason to block').
```

### 1.2 Команды `SG.AddPoint` / `SG.RemovePoint` получали имя команды как аргумент

AsaApi передаёт в callback **всю строку команды**, а не только аргументы:
`"SG.AddPoint Base 15"`. Старый код брал `args[0]` как имя зоны, поэтому:

* `cheat SG.AddPoint Base 15` создавал зону с именем **`SG.AddPoint`**
  (радиус при этом оставался по умолчанию, т.к. `std::stod("Base")` бросает
  исключение и оно молча глоталось);
* `cheat SG.RemovePoint Base` всегда отвечал `Zone not found`, потому что
  искал зону `SG.RemovePoint`.

Теперь первый токен отбрасывается, если он совпадает с именем команды
(регистр не важен). Дополнительно поддерживаются кавычки: `SG.AddPoint "Мой дом" 12`.

### 1.3 Незащищённая установка хуков (риск не загрузиться / уронить сервер)

В `Core/Private/Offsets.cpp` при отсутствии оффсета и `HooksDoNotThrow: false`
(значение по умолчанию в `Configs/config.json`) выполняется:

```
critical("Failed to get the offset of '{}'...") -> flush() -> Sleep(10000) -> throw;
```

Причём это `throw;` **без активного исключения** → `std::terminate()`.
Вызывается это и из `SetHook`, и из любых `NativeCall`/`*Field()` по имени.
Практический вывод: одно неверное имя = 10 секунд паузы и падение процесса.

Что сделано в плагине:

* все `SetHook` обёрнуты в `try/catch` и проверяют возвращённый `bool`;
* если хук `IsAllowedToBuild` не встал — плагин продолжает работать, пишет
  ошибку в лог и в `SG.List` показывает `IsAllowedToBuild=FAILED`;
* все «опасные» движковые вызовы (`GetAllActorsOfClass`, `GetActorBounds`,
  `GetAllPlayerStarts`, `BPLoadClass`, `IsPointNearSupplyCrateSpawn`) обёрнуты
  в SEH-обёртки;
* для отладки: поставьте в `Configs/config.json` `"HooksDoNotThrow": true` —
  промах по имени даст `error` в логе и `SetHook() == false` вместо падения.

Правильные имена хуков (скопированы дословно из сгенерированных заголовков
`AsaApi/Core/Public/API/ARK/PrimalStructure.h:813` и `:821`):

```
APrimalStructure.IsAllowedToBuild(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,FPlacementData&,bool,UE::Math::TVector<double>,bool,bool,APawn*)
APrimalStructure.FinalStructurePlacement(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,UE::Math::TRotator<double>,APawn*,FName,bool,FPlacementData&)
```

Обратите внимание: 6-й параметр — `TRotator` (`PlayerViewRotation`), а не вектор,
а `FPlacementData` в имени хука пишется как `&`, хотя в C++ это указатель
(такова конвенция генератора: `FString*` → `FString&`). Если после обновления
игры хук перестал вставать — проверьте имя в кэше оффсетов:

```bash
grep -o "APrimalStructure\.IsAllowedToBuild([^)]*)" cached_offsets.cache
```

### 1.4 Сообщения из конфига могли уронить хук (fmt + фигурные скобки)

`SendChatMessage`/`SendNotification` внутри вызывают `FString::Format` →
`fmt::format`, т.е. текст из конфига использовался как **строка формата**.
Если админ напишет в сообщении `{` или `}` (например `{tribe}`), `fmt` бросит
исключение прямо в хуке `IsAllowedToBuild`. Теперь любые runtime-тексты
передаются **аргументом** литеральной строки формата `"{}"` — фигурные скобки
безопасны, и код одинаково компилируется и со шимом fmt, и с современным fmt
из vcpkg (в старом коде были широкие литералы `L"..."`, которые с новым fmt
не собираются без `fmt/xchar.h`).

### 1.5 Зоны дропов считались по центру объёма (почти ничего не защищало)

`ASupplyCrateSpawningVolume` — это большой объём (эллипсоид/коробка) с
размером в тысячи юнитов, а радиус вокруг его начала координат покрывал
мизерную часть зоны. Теперь у каждого объёма один раз берутся границы
(`AActor.GetActorBounds`) и кэшируются как AABB; проверка точки — чистая
математика, без обращений к движку в хуке. Добавлен `VolumePaddingUU`
(расширить/сузить зону в юнитах).

### 1.6 Спам уведомлений

Хук `IsAllowedToBuild` вызывается десятки раз в секунду, пока игрок держит
структуру, поэтому сообщение в чат/на экран приходило на каждый тик.
Теперь есть cooldown `General.MessageCooldownSeconds` (по умолчанию 2 с)
и очистка таблицы при отключении игроков.

### 1.7 Белые/чёрные списки не срабатывали с именами предметов

Сопоставление шло только по имени актёра и цепочке классов, поэтому
`"PrimalItemStructure_SleepingBag_Hide"` из конфига **никогда** не совпадал
(в мире это `PrimalStructure_...`). Теперь матчинг идёт в трёх видах:
полное имя, цепочка классов и нормализованная форма, где выкинуты служебные
токены (`primalitemstructure`, `primalstructure`, `primalitem`, `primal`,
`blueprint`, `bp`, `item`, `structure`, `c`, `u`, `a`). То есть
`PrimalItemStructure_SleepingBag_Hide`, `PrimalStructure_SleepingBag_Hide_C_12`
и просто `sleepingbag` — равнозначны.

### 1.8 Потеря `config.json` при ошибке загрузки

`SaveCustomZonesLocked()` сериализовал `g_raw_config`, который при неудачной
загрузке оставался пустым `null` → первая же команда `SG.AddPoint` записывала
в `config.json` только `{"CustomZones": {...}}` и **стирала все настройки**.
Теперь запись запрещена, пока конфиг не загружен успешно.

### 1.9 Ненадёжный старт

* Хуки теперь ставятся в `Plugin_Init`, а не только из `BeginPlay` (не теряется
  ни одна попытка установки структуры).
* Данные сканируются из трёх мест: `Plugin_Init` (если сервер уже запущен),
  хук `AShooterGameMode.BeginPlay` и **сторожевой таймер** (раз в секунду
  проверяет `GetStatus()/GetWorld()`). Это закрывает случай hot-reload плагина
  (5-секундная автоперезагрузка AsaApi), когда `BeginPlay` уже не произойдёт.
* При смене мира (`UWorld` стал другим) кэш автоматически пересканируется.

### 1.11 Ошибка сборки: `check` — это макрос UE, а не имя переменной

Первая версия 2.0 не компилировалась: в коде были `check` как имя переменной и
как имя лямбды, а заголовки UE (их подключает AsaApi) определяют
function-like макрос `check(expr)` (`API/UE/Misc/AssertionMacros.h:218`).
Препроцессор превращает `check(a, b, c)` в вызов ассерта, и MSVC выдаёт каскад
из, казалось бы, не связанных ошибок:

```
check: необъявленный идентификатор
слишком много аргументов для вызова макроса, подобного функции, "check"   (×3)
синтаксическая ошибка: ;
синтаксическая ошибка: return
EnsureInit: неоднозначный вызов перегруженной функции
синтаксическая ошибка: }
синтаксическая ошибка: отсутствие ";" перед "}"
синтаксическая ошибка: отсутствие ";" перед "{"
{: отсутствует заголовок функции (возможно, используется формальный список старого типа)
```

Переменная переименована в `probe`, лямбда — в `consider`.
В комплекте есть проверялка на такие коллизии:

```
python Plugins/SpawnGuard/tools/check_macro_collisions.py
```

Она сверяет все идентификаторы `Main.cpp` со всеми 1700+ макросами из
`AsaApi/Core/Public` и падает, если найдено совпадение (сейчас — чисто).
Опасные имена, которые нельзя использовать как свои: `check`, `checkf`,
`checkCode`, `checkNoEntry`, `checkNoReentry`, `checkNoRecursion`, `verify`,
`verifyf`, `ensure`, `ensureMsgf`, `TEXT`, `LOCTEXT`, `UE_ASSUME`, `LIKELY`,
`UNLIKELY`, `CA_ASSUME` (плюс `min`/`max` из `Windows.h` — в файле они
`#undef`-нуты в начале).

### 1.13 Проверка имени хука по кэшу оффсетов + команда `SG.Hooks`

Строка хука зависит от билда игры (например, в параметрах может быть
`FPlacementData&` вместо `FPlacementData*` или отсутствовать `APawn*`), поэтому
жёстко зашитая строка рано или поздно перестаёт существовать в кэше:

```
[API][critical] Failed to get the offset of 'APrimalStructure.IsAllowedToBuild(...)'.
Requested by: plugin SpawnGuard
```

Теперь плагин **читает `ArkApi\Cache\...\cached_offsets.cache`** (это бинарная
сериализация `unordered_map<string, intptr_t>`, формат см. в
`AsaApi/Core/Private/Cache.h`) и:

* если точное имя есть в кэше — ставит хук по нему (обычный случай);
* если имя есть, но с другим списком параметров — пишет в лог **и ожидаемую
  строку, и то, что лежит в кэше**, и хук **не ставит** (сервер остаётся жив);
* если имени нет вовсе — пишет, сколько в кэше всего имён `APrimalStructure.*`
  и предлагает перекачать кэш (удалить `ArkApi\Cache` и перезапустить сервер);
* если файла кэша нет — работает как раньше (пробует дефолтную строку).

Появилась команда `cheat SG.Hooks`: показывает, какие хуки стоят, какие строки
к ним применены, путь к кэшу и **все** ключи кэша, начинающиеся с
`APrimalStructure.IsAllowedToBuild(`. Этого достаточно, чтобы после патча игры
починить плагин без пересборки: строка из кэша вписывается в
`General.HookNameOverrides` и делается `cheat SG.Reload`.

В комплекте также консольные утилиты для чтения кэша:
`tools/dump_offset_name.ps1` (PowerShell, работает на сервере без Python) и
`tools/dump_offset_name.py`.

### 1.12 RCON-перезагрузка работала в чужом потоке

Пакеты RCON обрабатываются отдельным потоком, а `RconReload` вызывал
`LoadConfigFile()` — то есть писал в `g_config`, переустанавливал хуки и лез в
мир из потока, который не является игровым. Это классический источник
редких крашей «после `SG.Reload` по RCON». Теперь RCON-команда только
выставляет `std::atomic<bool> g_pending_reload`, а саму перезагрузку делает
игровой поток в таймере (в течение секунды), отвечая при этом
`SpawnGuard: reload queued`. Заодно `SenderName()` читает имя отправителя под
мьютексом (и `PublishRuntime()` больше не вызывает его — `std::mutex` не
рекурсивный, это дало бы дедлок).

### 1.10 Мелочи

* добавлен `#include <algorithm>` (использовался `std::remove_if` без него);
* уведомление о причине блокировки пишется в лог только первые N раз
  (`General.DebugLogPlacements` / `DebugLogLines`);
* адаптер `AsaApi::GetPosition`, `bIsAdmin()`, `IsPointNearSupplyCrateSpawn`
  вызываются лениво — их не трогают, если правила этого не требуют;
* `SG.List` показывает состояние хуков и текущее значение блокировки
  (`block value=1`), а также предупреждает `(notify only!)` при `0`.

---

## 2. Сборка

### 2.1 Зависимость от fmt

Заголовки AsaApi сами используют fmt: `API/UE/Containers/UnrealString.h`
подключает `"fmt/format.h"`, а `FString::Format` реализован через
`fmt::format`. Без fmt-заголовков сборка падает на
`fmt/format.h: No such file or directory`.

В комплекте плагина есть папка **`fmt_shim/fmt/format.h`**: она перенаправляет
`"fmt/format.h"` на копию fmt, которая уже лежит внутри AsaApi
(`Logger/spdlog/fmt/bundled`, header-only). В `SpawnGuard.vcxproj` эта папка
уже добавлена в *Additional Include Directories* **первой**, поэтому ничего
устанавливать и линковать не нужно. Если правите проект вручную — путь
`$(ProjectDir)fmt_shim` обязан идти раньше любых путей с настоящим `fmt/`.

Вариант через vcpkg (`vcpkg.json` уже в проекте: `fmt`, baseline
`ed8445dd2a9ba44e0e3ace4e952b2e33130082d9`, overlay-триплет из
`AsaApi/vcpkg_custom_triplets`) работает, но не подключайте `fmt/format.h`
напрямую из vcpkg: в одной единице трансляции окажутся две разные версии fmt
(vcpkg и встроенная в spdlog) — это конфликт. Шим оставляет одну версию.

### 2.2 Сборка в Visual Studio

1. Откройте `AsaApi.sln`, добавьте `Plugins\SpawnGuard\SpawnGuard.vcxproj`
   (правый клик по решению → Add → Existing Project).
2. Соберите `Release|x64` (нужен toolset **v143 / 14.39.33519**, как у AsaApi).
3. Post-build копирует рядом с DLL: `SpawnGuard.dll.ArkApi`,
   `PluginInfo.json` и (если ещё нет) `config.json`.

### 2.3 Грабли компиляции

Если MSVC выдаёт «слишком много аргументов для вызова макроса, подобного
функции», «необъявленный идентификатор» или каскад «синтаксическая ошибка» —
почти наверняка какое-то ваше имя совпало с function-like макросом из
заголовков UE (см. п. 1.11). Быстрая проверка перед сборкой:

```
python Plugins/SpawnGuard/tools/check_macro_collisions.py
```

Ещё пара моментов при правках:

* не убирайте `#undef min` / `#undef max` в начале файла — их определяет
  `Windows.h`, который тянет за собой SDK;
* тексты для игроков передавайте как аргумент `"{}"`, а не как строку формата:
  `SendChatMessage(pc, sender, "{}", text)` — иначе фигурная скобка в тексте
  из `config.json` бросит исключение внутри хука;
* `SetHook` всегда оборачивайте в `try/catch` (см. п. 1.3).

### 2.4 Установка

```
<Server>/ArkApi/Plugins/SpawnGuard/
    SpawnGuard.dll.ArkApi     <- переименованный SpawnGuard.dll
    PluginInfo.json
    config.json
```

`PluginInfo.json`: `"Version": 2.0`, `"MinApiVersion": 2.03`.

---

## 3. Команды

| Команда | Описание |
|---|---|
| `cheat SG.Reload` | перечитать `config.json` и пересканировать точки (есть и RCON `SG.Reload`) |
| `cheat SGReloadConfig` | синоним `SG.Reload` (совместимость со старой версией) |
| `cheat SG.Rescan` | принудительный полный рескан (сбрасывает «заморозку» заметок/объёмов) |
| `cheat SG.List` | счётчики (спавны, объёмы, ящики, заметки, зоны), состояние хуков, значение блокировки, список зон |
| `cheat SG.Near` | ваши координаты и ближайшая защищённая точка (в фундаментах) |
| `cheat SG.Hooks` | состояние хуков, применённые имена, путь к кэшу оффсетов и все ключи кэша для `IsAllowedToBuild` |
| `cheat SG.AddPoint <Имя> [Радиус_в_фундаментах]` | добавить кастомную зону в текущей позиции |
| `cheat SG.RemovePoint <Имя>` | удалить кастомную зону |

Команды работают только у админов (обычный консольный чит-путь).

---

## 4. Конфиг

### `General`

| Ключ | По умолчанию | Значение |
|---|---|---|
| `Enabled` | `true` | глобальный выключатель |
| `ChatSenderName` | `SpawnGuard` | имя отправителя в чате |
| `LogDiscoveredPoints` | `true` | писать в лог найденные точки/классы |
| `RefreshSpawnPointsSeconds` | `30` | как часто обновлять динамику (ящики) |
| `BlockReturnValue` | `1` | что вернуть движку вместо причины «нельзя»; `0` = только уведомлять |
| `MessageCooldownSeconds` | `2.0` | антиспам сообщений на игрока |
| `DebugLogPlacements` | `false` | писать в лог первые блокировки (структура, зона, что вернул движок) |
| `DebugLogLines` | `8` | сколько таких строк писать |
| `EnforceOnFinalPlacement` | `false` | дополнительный хук `FinalStructurePlacement` (см. ниже) |

### Секции зон

`SpawnPoints`, `SupplyDrops`, `ExplorerNotes`, `CustomZones` понимают общие ключи:
`Enabled`, `IgnoreAdmins`, `BlockMode` (`BlockAll` | `BlockOnlyBlacklisted`),
`RangeInFoundations`, `StructuresWhiteList`, `StructuresBlackList`, `Messages`
(`SendNotification`, `SendChatMessage`, `NotificationText`, `ChatMessageText`,
`NotificationColor`).

Дополнительно:

* `SupplyDrops`: `ProtectActiveCrates`, `ProtectDropVolumes`, `VolumePaddingUU`,
  `UseEngineDropSpawnCheck`, `UseEngineDropSpawnCheckOnPreview`;
* `ExplorerNotes`: `BaseClassNames` (пути/имена базового класса заметок) и
  `NameContains` (подстроки цепочки классов для резервного сканирования);
* `CustomZones`: `DefaultRangeInFoundations` и массив `Zones` (его ведёт плагин
  при `SG.AddPoint` / `SG.RemovePoint`; `config.json` перезаписывается
  аккуратно, остальные ключи сохраняются).

**Важно:** `config.json` разбирается строгим JSON-парсером — комментариев и
висячих запятых в нём быть не должно, иначе плагин откажется читать файл
(и не будет его перезаписывать).

### `EnforceOnFinalPlacement` — второй барьер (по умолчанию выключен)

Если по какой-то причине первого барьера не хватает, плагин умеет
дополнительно хукать `APrimalStructure.FinalStructurePlacement` и возвращать
`0` (отказ) до фактической установки. По умолчанию выключено, потому что
в сгенерированном заголовке этот метод неоднозначен: `NativeCall` объявляет
векторы/ротаторы указателями, а имя в кэше оффсетов записано без `&`, т.е. как
значения (`APrimalStructure.FinalStructurePlacement(APlayerController*,UE::Math::TVector<double>,UE::Math::TRotator<double>,UE::Math::TRotator<double>,APawn*,FName,bool,FPlacementData&)`).
Неверная сигнатура = падение сервера при установке структуры, поэтому включайте
это только осознанно и после проверки имени в `cached_offsets.cache`.

---

## 5. Как ищутся заметки

1. **Точное сканирование по классу.** Плагин резолвит `UClass`
   `/Game/PrimalEarth/CoreBlueprints/ExplorerChest/ExplorerChest_Base.ExplorerChest_Base_C`
   через `UVictoryCore::BPLoadClass` и вызывает `GetAllActorsOfClass` — движковый
   `IsA` находит все производные варианты (записки, досье, глитчи, руны, HLN-A).
   Если резолв по пути не удался, базовый класс «ловится» из цепочки наследования
   первого найденного актёра-заметки.
2. **Резервное широкое сканирование.** Перебор всех актёров мира со сравнением
   полной цепочки классов (`aactor > explorerchest_base_c > bp_..._c`) с
   `NameContains` (вердикт кэшируется по классу). Нужен для кастомных карт.

После находки сканирование «замораживается» (заметки статичны), но продолжается
до 8 пустых попыток, пока класс не определён, и до 40 попыток в худшем случае.
Диагностика в `ArkApi.log`:

```
SpawnGuard: explorer note base class resolved
SpawnGuard: note scan by ExplorerChest class: N actors
SpawnGuard: broad note scan: N actors, M matched filters
SpawnGuard: note-candidate class: ...   <- подсказки для NameContains
```

Если заметок нет: `cheat SG.Rescan`, посмотреть в логе классы-кандидаты,
добавить нужную подстроку в `ExplorerNotes.NameContains`, затем `cheat SG.Reload`.

---

## 7. Если в логе `Failed to get the offset of ...`

Это значит, что строки, которую плагин передаёт в `SetHook`, нет в кэше оффсетов
**вашего** билда игры.

Что при этом происходит в ядре (`AsaApi/Core/Private/Offsets.cpp`):

```cpp
if (!offsets_dump_.contains(name)) {
    Log::GetLog()->critical("Failed to get the offset of '{}'...", ...);
    Log::GetLog()->flush();
    if (hooks_do_not_throw_) return nullptr;
    Sleep(10000);
    throw;              // <-- без активного исключения => std::terminate
}
```

То есть при `"HooksDoNotThrow": false` (значение по умолчанию) сервер
замирает на 10 секунд и падает. **Поставьте `true`** в `config.json` рядом с
`ArkAscendedServer.exe` (в репозитории этот файл лежит как `Configs/config.json`),
внутри блока `settings`:

```json
{
  "settings": {
    "HooksDoNotThrow": true
  }
}
```

Тогда промах по имени даёт только запись `[error]` в логе (`ArkApi.log` в папке
`<server>\ShooterGame\Win64\logs\`).

Начиная с версии 2.0.1 плагин до вызова `SetHook` сверяет имя с кэшем, поэтому
при несовпадении хука просто не будет (плагин живой, блокировка выключена,
в логе — точная диагностика), а не «критикал + пауза».

### Как посмотреть, что лежит в кэше

Из игры: `cheat SG.Hooks` — команда печатает все подходящие ключи.

На сервере, без перезапуска (PowerShell, из папки `...\ShooterGame\Binaries\Win64`):

```powershell
.\ArkApi\Plugins\SpawnGuard\tools\dump_offset_name.ps1 -Pattern IsAllowedToBuild
```

Или вручную, одной строкой:

```powershell
Get-ChildItem -Path .\ArkApi -Recurse -Filter cached_offsets.cache | ForEach-Object {
  $fs=[IO.File]::OpenRead($_.FullName); $br=New-Object IO.BinaryReader($fs)
  while ($fs.Position -lt $fs.Length) {
    $len=$br.ReadUInt64()
    if ($len -eq 0 -or $len -gt 1048576) { break }
    $key=[Text.Encoding]::UTF8.GetString($br.ReadBytes([int]$len)); $null=$br.ReadInt64()
    if ($key -like "*IsAllowedToBuild*") { $key }
  }
  $br.Dispose(); $fs.Dispose()
}
```

### Что делать с результатом

1. **В кэше нет ни одного ключа `APrimalStructure.IsAllowedToBuild(`** —
   функция в этом билде называется иначе или её нет. Смотрите шире:
   `dump_offset_name.ps1 -Pattern "AllowedToBuild"` и `-Pattern "APrimalStructure."`.
   Если кэш вообще пустой/битый — удалите папку `ArkApi\Cache` и перезапустите
   сервер: ArkApi скачает кэш под текущий `ShooterGame.exe`.
2. **Имя есть, но список параметров другой** — проверьте, чем он отличается
   (`FPlacementData&` против `FPlacementData*`, наличие `APawn*` в конце и т.п.).
   Если отличие только в последнем параметре, обновите `DECLARE_HOOK` и
   `Hook_...` в `Main.cpp` под новую сигнатуру и пересоберите плагин.
3. **Имя из кэша полностью подходит** (совпадает с тем, что компилировали) —
   впишите его в `config.json`:

```json
"General": {
    "HookNameOverrides": {
        "IsAllowedToBuild": "APrimalStructure.IsAllowedToBuild(...скопировать точно...)",
        "FinalStructurePlacement": "",
        "GameModeBeginPlay": ""
    }
}
```

   и выполните `cheat SG.Reload`. Пересборка не нужна.

> ⚠️ `HookNameOverrides` — это «force». Плагин предупредит в логе, если строки
> нет в кэше, но всё равно её использует. Если список параметров строки не
> совпадёт с `DECLARE_HOOK`, сервер упадёт при первой установке структуры,
> поэтому копируйте строку из кэша целиком и сверяйте параметры.

## 8. Известные ограничения

* Плагин блокирует установку **на сервере**: призрак структуры у клиента не
  краснеет (клиентская проверка идёт в процессе игрока), но установка
  отклоняется и игрок получает сообщение.
* Спавны берутся из `UVictoryCore.GetAllPlayerStarts`, при неудаче — из
  `APrimalGameMode.PlayerStarts`. На некоторых картах часть точек спавна
  (динамические/миссионные) может не попадать в список — тогда добавьте
  кастомную зону.
* Обсидианы/терминалы данностей в SDK не имеют C++-классов (они Blueprint):
  при необходимости закрывайте их кастомными зонами.
* Порядок проверок в хуке: сначала дешёвые кэшированные точки (спавны, ящики,
  заметки, зоны), затем объёмы дропов, затем движковая проверка
  `IsPointNearSupplyCrateSpawn` (по умолчанию только на финальной установке).
