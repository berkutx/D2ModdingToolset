# Проверить старт ОХ через транспорт лобби

Регрессия воспроизводит остановку на раннем `CJoinGameMsg` настоящим кодом
`lobby_transport.cpp`. До исправления исходники `42081ba2` возвращают Abort;
после исправления последовательность проходит. Это **не игровая приёмка**:
native dispatch, состояние CMidClient, UI loop и сеть подставлены тестом.

## Повторить проверку

Из MSVC x86 developer shell, PowerShell 7, корень этого checkout:

```powershell
./tests/run-simturns.ps1 -OutputDirectory ./artifacts/simturns-check
```

Обычный runner включает старые unit-тесты и новый startup regression в Debug
и Release. Последний компилирует продуктовые `lobby_transport.cpp`,
`coordinator_port.cpp`, `protocol.cpp`, `control_client_core.cpp`, `turn_context.cpp`.
Проверки не зависят от `assert` и работают с `NDEBUG`. После ошибки компиляции
старый EXE не запускается. Transcript содержит пути и SHA256 исходников.

Чтобы независимо повторить красный тест без правки текущих исходников:

```powershell
$baseline = Join-Path $env:TEMP ('oh-baseline-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $baseline | Out-Null
$archive = Join-Path $baseline 'source.zip'
git archive --format=zip -o $archive 42081ba2182366446fb136fc509ff0db39df40f0 -- mss32/include mss32/src/simturns
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $baseline 'source')
./tests/run-simturns-lobby-startup-regression.ps1 `
  -ProductionRoot (Join-Path $baseline 'source') `
  -OutputDirectory (Join-Path $baseline 'red') -Configuration Debug
```

Ожидается ненулевой exit code именно с сообщением
`observed join startup burst emitted protocol Abort at zero-handler CJoinGameMsg(62)`.
Ошибка компиляции не является воспроизведением. Для старой PR10 API callback
без диагностического аргумента этот PR11 fixture не подходит: baseline указан явно.

## Evidence → Finding → Path

### E1. Исходный сбой

Сохранённый журнал: `artifacts/oh-repro-20260924-065703/mss32.log`, SHA256
`b1eff2fd93c368cd0561a9cd7032508caaa33b0a21f5067eeefc86b4ca867ca2`.
24 сентября, 06:53:18.812, join PID27924, room6/epoch5, ticket29:
`CJoinGameMsg`, 62 байта, sender1, client receiver, `policy=pass`,
`dispatched=true`, `handler_count=0`, `awaiting_drain=13`.
Затем local Abort и remote Abort хоста; процессы продолжили работу.
Это отказ протокола, не доказанное падение EXE.

F1: ноль обработчиков ошибочно считался неисправной доставкой во всех случаях.
Путь: host startup broadcast → join ещё в меню → native dispatch возвращает0 →
completion классифицирует Failed → lobby Abort.

### E2. Штатные обработчики и формат

Russobit EXE SHA256
`1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb`.
IDA: JoinGame регистрирует только CMidClient (`0x40BA45`, callback `0x40C5A1`).
Меню CMenuLobby регистрирует собственные MenusAnsInfo (`0x4E2233`) и PlayerList
(`0x4E227E`); их отсутствие нельзя оправдать отсутствием CMidClient.
Serializer JoinGame `0x47D334`, string helper `0x47E5F2`:

| Поле | Смещение | Размер |
| --- | ---: | ---: |
| Заголовок | 0 | 44 |
| Player ID | 44 | 4 |
| Длина имени N, включая NUL | 48 | 4 |
| Имя в игровой кодировке | 52 | N |
| Категория лорда (не раса) | 52+N | 4 |

F2: размер не фиксирован на62; равен56+N. Валидатор требует ненулевой player ID,
N1..256, точную общую длину, единственный конечный NUL и категорию лорда0..2.
Путь: header → длина по разности → имя → хвост; непроверенная длина не используется
для чтения хвоста. Проверка не пытается привязать player ID к аккаунту по имени.

Полные42 ответа IDA сохранены локально:
`artifacts/oh-startup-native/startup-native-evidence.json`, SHA256
`62edf46cca4b177f503caba18767fe49787796822688be9683039daee4500326`.
Scope и адресная расшифровка рядом в отчёте native-аудита. Бинарник не патчился.

### E3. Пробел прежних тестов

Прежний unit fixture вручную подтверждал выполнение через `reportActionResult`.
Локальный test-only coordinator использует DirectPlay и не проходит через
lobby binding/staging/completion. Поэтому его успех не проверял этот переход.

F3: нужен regression на реальном транспорте, отдельно от unit модели.
Путь: Arm → native ticket → реальное staging → подставленный native результат →
реальный completion → очередь UI → native fence → реальный CoordinatorPort.

Startup fixture сохраняет порядок и размеры18 сообщений из журнала. Сырых
payload в журнале нет: JoinGame body синтетический, прочие результаты dispatch
(кроме зафиксированного JoinGame0) заданы тестом.13 Applied воспроизводят
суммарный pending drain; это не побайтовый replay и не восстановление всех
реальных handler counts.

## Граница исправления

Разрешается только штатно доставленный Unhandled JoinGame на клиентском endpoint
джойнера, от server1, до собственного NewScenario/StartScenario, в одной и той же
проверенной pregame generation без CMidClient до и после native dispatch.
Он завершает только свой ticket как Filtered. Applied сохраняет ожидание
стратегической очереди, Failed/Drop остаются ошибкой. Направленный BeginTurn,
MenusAnsInfo и PlayerList не получают этого исключения.

Отрицательные тесты проверяют фазы, generation, старый binding, sender/role/endpoint,
обязательные сообщения и fence: control frame не проходит раньше применённого
native сообщения. Проверки формы JoinGame отдельно входят в unit suite.

## Что ещё требуется

Сборка и offline regression не заменяют запуск двух настоящих клиентов через
лобби с `DisplayErrors=1`. Требуются штатный вход, генерация, старт обоих,
несколько ходов, бой и переход в обычные ходы. Ранее выполненная локальная кампания
не доказывает работоспособность лобби; повтор18-case требует её точной fixture-карты.
Сетевой live-прогон на этой итерации не выполнен: операция входа остановлена
проверкой разрешений до передачи учётных данных. Настройки и чужие матчи не менялись.
