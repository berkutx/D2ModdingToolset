# ОХ: ранние сообщения до игрового клиента джойнера

24 сентября 2026. [Область и авторизация](2026-09-24_oh-refresh-scope.md).
Сборка и консольные регрессии не заменяют живой прогон двух клиентов с
`DisplayErrors=1`. Установка DLL также не означает прохождение этого прогона.

## Evidence

### E-1 — последовательность отказа

Приватный снимок PR11: `artifacts/oh-repro-20260924-053329/mss32.log`, SHA256
`fba10e47c629f52570f076a7759dd8cf5cf0966f3988713108e16bcca3bb9ee2`.
Для локального повторения из checkout PR11:

```powershell
rg -n '^09/24/26 05:27:.*(simturns-diag|NewScenario|StartScenario|ReqStartGame|MATCH_ENDED|clearNetworkState|leaveRoom)' artifacts/oh-repro-20260924-053329/mss32.log
```

Комната5/epoch4: host PID12216, join PID17668. Хост отправляет ReqStartGame
05:27:20.732 UTC; собственные NewScenario/BeginTurn/StartScenario уже проходят.
У join первым отказывает `CRefreshInfo`,23896bytes,server1→client,
native site0x402ca7,Pass/dispatched/handler_count0,05:27:21.464.
Join отправляет Abort; host получает remote_abort и MATCH_ENDED в21.561,
очищает network state в21.600 и выходит из комнаты в21.608.
Сервер отдельно подтверждает Client aborted simultaneous turns, не timeout.
Часы сервера/клиента не считаются синхронными. Общий клиентский лог содержит
перемешанные/частично перезаписанные строки; отсутствие строки не доказательство.

### E-2 — ограничения существующего пути

`netintercept.h::nativeDispatchResult`: ноль — число совпавших обработчиков,
не ошибка возврата отдельного callback. Exact Connect ранее исправлен отдельно.
`lobbyrestart.cpp::checkLobbyRestartClientMessage` уже документирует ранние
Refresh/Erase broadcasts до собственного NewScenario джойнера и последующий
полный адресный снимок; этот receive-фильтр включён только при активном111.
`netcustomservice.cpp::processPendingMatchEnd` объясняет возврат хоста в меню.
Content hash: n/a, tracked source. Повторение:

```powershell
rg -n 'nativeDispatchResult|pre-scenario|full object snapshot|MID_STARTMENU' mss32/include/netintercept.h mss32/src/lobbyrestart.cpp mss32/src/netcustomservice.cpp
```

### E-3 — происхождение turn.lua

Начало обычного native хода вызывает `beginTurnOrig`, затем необязательный
`Scripts/turn.lua::processTurnStart(player)`. Hook не включается условием ОХ.
В тестовом Copy файла нет. Общий loader допускает отсутствие optional файла,
но caller ошибочно дописывал `[TURN] failed to load processTurnStart`.
Сам callback, регистрация и эта строка унаследованы из
347cd3a78abde5c051d15816e6e28b7ea6b1d6a2 (17 мая 2026).
PR8/PR10/PR11 до исправления содержат один blob turnhooks.cpp:
`b9b7f395ac57d88cde958c6e79571ecef35eb017`. Повторение:

```powershell
git show 347cd3a78abde5c051d15816e6e28b7ea6b1d6a2 -- mss32/src/turnhooks.cpp
```

### E-4 — проверка native bootstrap и размера Refresh

Статически проверен точный Russobit EXE (PE32), SHA256
`1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb`.
Локальные raw ответы IDA: `artifacts/oh-refresh-native/targeted-refresh-evidence.json`,
SHA256 `4c1d339466b91174dc5ad9ea48176feb28765227051f5c03264f755837cd46de`.
Воспроизведение в той же disposable базе:
`artifacts/oh-refresh-native/collect-targeted-evidence.ps1` (21/21 ответ).
EXE и приватные логи в Git не включаются.

NewScenario: ctor0x47E463, vtable0x6D50BC, RTTI0x78FC88; отправка адресату
0x421D33/0x421D3A, затем полный снимок 0x421D5A→0x4218CA→0x42972F.
Минимальный сериализованный Refresh — 52 байта: header44 (0x55CC9F),
scenarioID4 (0x47E666), count4 (0x605304→0x604E46). При isExpansionContent
добавляется ещё4 (0x47C5D2); пустая коллекция 0x5EFFBA не пишет хвост.
Поэтому общий нижний порог — 52, а не56. Это проверка envelope, не парсер
всех объектов; native dispatch и обработка ошибок payload сохранены.
Дизассемблер подтверждает порядок полного снимка, а конкретный ранний пакет — E-1.

### E-5 — соседний startup BeginTurn

Локальный raw `artifacts/oh-refresh-native/targeted-beginturn-evidence.json`,
SHA256 `4ed08e13dbe0e50ae7af5d42e4d4bfaf4df2008fbee9f9fd8137e89863755f54`;
повтор: `artifacts/oh-refresh-native/collect-beginturn-evidence.ps1` (12/12).
Тот же E-1 содержит у join UI20184 startup broadcast BeginTurn в05:27:21.465
после первого Abort. Его handler_count в этом прогоне уже не установлен;
следующий отказ нельзя выдавать за отдельно воспроизведённый.

Native CNMMap ctor0x40FD60 безусловно регистрирует оба callback:
Refresh0x40FD9F→0x4102C0 и BeginTurn0x40FF47→0x4102B0 через add0x55BAA9.
Другой BeginTurn member-handler принадлежит CMidClient: vtable0x6CECFC,
adapter0x40F079, allocator0x40E0C8, registration0x40BAE6→callback0x40CC7C.
Отдельного menu-handler нет. Отсутствие обработчика Refresh и CMidClient
объясняет отсутствие обоих маршрутов BeginTurn в том же предыгровом состоянии.
В исходниках rxGate сохраняет startup proof, но возвращает Pass, не Consume.
Поэтому только Refresh-исправление оставило бы следующий zero-handler отказ.

## Findings и Path

- F-1: validated/high confidence/n/a_re, E-1/E-2/E-4,
  `lobby_transport.cpp::nativeReceiveCompleted`: ранний нормально проигнорированный
  Refresh приводит к остановке ОХ. Это отдельный случай после исправленного Connect.
- F-2: validated/high confidence/n/a_re, E-3, `turnhooks.cpp`: отсутствующий
  optional script ошибочно выглядит обязательной зависимостью. Прямой причины
  Abort в этом hook нет; исходный native ход уже выполнен.
- F-3: inferred/high confidence/n/a_re, E-5, следующий ранний broadcast BeginTurn
  требует того же узкого предыгрового признания. Native регистрации подтверждены,
  но отдельный следующий Abort в игре не воспроизводился.
- P-1, path_type=callflow: host startup broadcast [E-1] → join без обработчика
  [E-1/E-2] → Unhandled становится Failed [F-1] → Abort → серверный MATCH_ENDED
  → автоматический возврат хоста в лобби [E-1/E-2].

## Исправление и проверка

Native dispatch не пропускается. Только normally Unhandled exact Refresh
либо первый broadcast BeginTurn56 с полями{addressee0,sequence1,active!=0}
от server1 клиенту с ролью join получает Filtered, если до/после dispatch
одинаковое ненулевое предыгровое поколение: UI thread, нет CMidClient,
phaseGame, старта координатора и teardown. Per-Binding latch закрывается
при staging собственного NewScenario либо StartScenario; completion проверяет
его синхронно до постановки UI задачи. Внутри той же Binding latch не сбрасывается.
Это не общее исключение для Refresh/Erase/нулевых callbacks. Applied/Failed/Drop,
causal completion, synthetic path, старые bindings и native fence не ослаблены.
Directed BeginTurn не разрешён. Существующая проверка startup proof в rxGate
не меняется: неверный маршрут, формат и повтор дают Drop/Failed, не Filtered.

Optional turn.lua: отсутствие файла тихо пропускается, ошибки существующего
скрипта остаются видны. Lua/INI/диалоги не создаются и не изменяются.

Регрессии из x86 Developer PowerShell:

```powershell
./tests/run-simturns.ps1 -OutputDirectory ./artifacts/oh-refresh-tests
./tests/run-turn-script-optional.ps1
```

Проверены sender/endpoint/role/type/RTTI/длина, монотонная граница сценария,
смена поколения, переход между staging/completion, сохранение Connect,
точный startup broadcast, отказ directed/duplicate-policy-failure,
Filtered без обхода предыдущего native ticket и повторные completion.
Отдельный source-тест проверяет optional Lua и отрицательные контроли.
Нативная сборка обязательна для обеих веток; PR11 сохраняет свою диагностику.

## Timeline

- 05:23 UTC: установлено предыдущее Connect-исправление.
- 05:27 UTC: воспроизведён следующий ранний отказ на Refresh; лог сохранён.
- Затем сопоставлены клиентский/серверный логи и установлен автоматический выход.
- По запросу пользователя подготовлены узкое исправление и регрессии.
- Живая приёмка: после установки повторить первый запуск, вход/выбор расы,
  загрузку обоих клиентов, первый одновременный ход и день объединения.
