# Перенос ОХ на prepared matches: происхождение и границы

## Что проверяется

Цель — производственное ОХ поверх существующего лобби-соединения, без зависимости
от тестхарнеса. Сервер описывается контрактом, но не реализуется в этом PR.
Универсальный харнес для локального рилея и настоящего лобби согласован отдельным PR.

Исходная ветка ОХ: `feature/simturns-on-test-harness`, commit
`b7637a031e307e19b2df6d817783f23dabce540e`. Её база минимального харнеса:
`88760a7f0b4a6209647f1348f3450e8a48cdfc01`.
Новая ветка: `codex/prepared-simturns`; base PR — `codex/prepared-matches`.
Перенос начат с опубликованного `c22d29bf`; во время работы база продвинулась до
`814e6488c8f48971434afa2048f40d623db3511d` (выбор последнего локального шаблона).
Незакоммиченные изменения других worktree не являются источником этого PR.

## Карта решений по исходному diff

Полный исходный diff — 138 файлов; он включает восстановленный live-харнес,
который нельзя целиком cherry-pick в производственный PR.
Проверенные группы: 30 native simturns, 31 testdrv, 26 общих native/project/tests,
13 файлов координатора, 36 остальных tools, один документ и один workflow.

| Группа исходных изменений | Решение | Причина / сохранённый контракт |
| --- | --- | --- |
| `protocol`, `control_client_core`, `turn_context` | Сохранить | Игровой v8 и проверки последовательности не меняются |
| `controller`, `state` | Перенести с map-scoped lifecycle | Вместо одного pipe-процесса поддержать teardown и новую карту в том же клиенте |
| `patches`, `engine_hooks` | Сохранить игровые алгоритмы, полный rollback | Патчи старой карты не должны пережить новую обычную комнату |
| `day_scope`, `spell_timing`, `battle_compat` | Сохранить, ограничить актуальной ролью/режимом | Один клиент может сначала быть хостом, затем join |
| `russobit_sites`, `executablefingerprint` | Сохранить точную идентификацию и проверенные адреса | Новый неподтверждённый ABI не вводится |
| `coordinator_port` | Заменить транспортную реализацию | Синхронная постановка в RakPeer Send вместо pipe-worker; тот же контрольный автомат |
| `config`, `pipeclient`, `named_pipe_endpoint` | Не переносить | Production-room не управляется локальной средой или тестовым pipe |
| `netintercept`, `uiframedispatcher` | Узкие production-точки | Штатная доставка, UI-thread применение, принадлежащая карте отложенная работа |
| `main`, `hooks`, `midobjectlockhooks`, `phasegamehooks`, `midclientcore` | Условные подключения | `D2_SIMTURNS`; сохранены подготовленные матчи и исходный restart flow |
| `netcustomplayerclient/server` | Условные post-original TX/RX точки | Не обходить обычный транспорт |
| `phasegame::clientTakesTurn`, тестовый movement API | Не переносить | Только интерфейс автоматизации сценариев |
| `batlogichooks`, `battlemsgdatahooks` tracing | Не переносить | Исходные добавления — `D2_TESTDRV`, не игровая реализация ОХ |
| `include/testdrv`, `src/testdrv` | Отдельный следующий PR | Один универсальный харнес, не скрытый второй вариант ОХ |
| `tools/relay`, `tools/test`, fixtures и live PS-драйверы | Отдельный следующий PR | Не менять осмысленные сценарии при транспортном переносе |
| `tools/simturns-relay` | Сохранить эталон и unit/pipe tests | Проверяемый серверный автомат; README явно отделяет его от новой DLL |
| Portable control-client test | Сохранить в `tests/` | 15 исходных протокольных сценариев |
| Исходный test-harness workflow и isolation tests | Не переносить | В текущем PR отсутствует D2_TESTDRV |
| MSBuild / CI | Явная независимая опция | Debug и Release, ОХ включены/выключены |

Новые изменения сверх донора: `lobby_wire`, `lobby_transport`, native-apply barrier,
room capability/admission, teardown hooks, тесты адаптера и серверный контракт.
Наличие этих изменений означает, что старые live-результаты нельзя присвоить новой DLL.

## Evidence

### E-001 — воспроизводимый список исходных изменений

source_type: command; observed_at: 2026-09-22; content_hash: n/a.

В репозитории с обеими ветками:

```powershell
git diff --stat 88760a7f b7637a03
git diff --name-status 88760a7f b7637a03
git show b7637a03:mss32/src/simturns/controller.cpp
```

raw_excerpt: исходный diff включает native ОХ и `mss32/src/testdrv`, `tools/test`.
Сравнение проведено с clean donor, отдельно от старых временных DLL и отчётов.

### E-002 — исполняемый протокольный эталон

source_type: file; observed_at: 2026-09-22; content_hash: git blob донора;
source_ref: `tools/simturns-relay/src/coordinator.js`, `mss32/src/simturns/control_client_core.cpp`.

```powershell
git diff b7637a03 -- tools/simturns-relay/src tools/simturns-relay/test
git diff b7637a03 -- mss32/src/simturns/protocol.cpp mss32/src/simturns/control_client_core.cpp mss32/src/simturns/turn_context.cpp
```

Ожидаемый diff этих реализаций пуст; README транспорта меняется отдельно.

### E-003 — native startup и граница применения

source_type: file; observed_at: 2026-09-22; content_hash: проверяется по commit PR;
source_ref: `menucustomnewskirmishmulti.cpp`, `netcustomplayer.cpp`, `netintercept.cpp`,
`simturns/controller.cpp`, `simturns/lobby_transport.cpp`.

```powershell
rg -n 'createServer|CreateRoom_Callback' mss32/src/menucustomnewskirmishmulti.cpp
rg -n 'stageNativeReceive|NativeReceiveResult|nativeApplied|onPhaseGame' mss32/src/netintercept.cpp mss32/src/simturns
```

raw_excerpt: создание сервера, получение игрового пакета и применение команды —
разные события. Порядок packet callbacks сам по себе не является ACK движка.

### E-004 — тождество ReceiveMessage buffer в точном EXE

source_type: binary/disassembly; observed_at: 2026-09-22;
content_hash (EXE SHA256):
`1375cdef09ec470ee64fe5693f3fb734d7c69fb215212311d997f792b258a642eb`.
source_ref: caller `0x402BD3` и `0x4338BE`, dispatcher `0x55B948`.

Для повторения открыть EXE указанного SHA256 в IDA, дизассемблировать оба caller
и сравнить аргумент buffer у virtual slot `+0x18` с аргументом dispatcher.
Байты из базы IDA дополнительно сравнены с файлом EXE по PE section mapping:
56 байт с `0x402C74` и 146 байт с `0x4338E1` совпали полностью.

- Первый caller сохраняет `dword_7B7A24` в ESI; тот же pointer передаёт
  в ReceiveMessage (`0x402C89`), затем через `push esi` (`0x402C9F`)
  в dispatcher (`0x402CA7`).
- Второй caller сохраняет адрес stack buffer в `[ebp-0x8000C]` и передаёт его
  в ReceiveMessage (`0x433921`), затем повторно из той же переменной
  (`0x433962`) в dispatcher (`0x43396E`).

Это подтверждает pointer identity для регистрации native completion, а не только
совпадение содержимого пакета. Проверка статическая, не live-прогон DLL.

## Findings и путь применения

F-001 (design, validated, confidence high; E-001/E-002): перенос полного donor diff
включил бы тестовые fixtures и pipe-policy в production. Разделение сохраняет
игровой автомат, исключая харнес по явно согласованной границе.

F-002 (design, validated по исходникам, confidence high; E-003): Arm может прийти
после native создания serverLogic; привязка лишь в конструкторе теряет хост.
Используется существующая типизированная цепочка native объектов при входе в карту.

F-003 (design, validated по исходникам, confidence high; E-003): control может
обогнать ещё не применённую игровую команду. Отдельная отметка завершения native
обработки и упорядоченный адаптер заменяют предположение о сетевой задержке.
Динамическая проверка этого пути через настоящий сервер остаётся обязательной.

F-004 (reverse_algo, validated, confidence high; E-004): receipt можно сопоставлять
по сырому buffer pointer до вызова исходного dispatcher; его завершение не следует
публиковать в момент успешного ReceiveMessage.

P-001 (callflow; E-002/E-003, F-002/F-003): авторизованная комната → Arm/ArmAck →
обычная загрузка карты → native player binding → SessionPlan/bootstrap →
пара post-original EndTurn свидетельств → EngineAction/ActionResult →
подтверждённое объединение либо terminal teardown. Ни один шаг не требует testdrv.

Оставшийся риск: серверный адаптер и новая DLL не проверены совместным живым
прогоном двух клиентов. Исторические 18/18 подтверждают только исходную реализацию.
Текущие воспроизводимые команды проверок приведены в
[руководстве](SIMULTANEOUS_TURNS.md#проверить-протоколы).
