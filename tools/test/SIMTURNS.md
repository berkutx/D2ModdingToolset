# Проверка одновременных ходов

Актуальный вход и границы Local/Lobby: [один харнес](../../docs/TEST_HARNESS.md).
Описанные ниже 18 и дополнительные fixture-сценарии используют Local.
В этой ветке `D2MSS_SIMTURNS*` выбирает только test-only локальный адаптер;
боевое ОХ через лобби включается сборкой и серверным Arm, не этими переменными.

Это сценарии общего [test harness](README.md), а не отдельная реализация harness.
`D2_TESTDRV` включает наблюдение и команды в DLL; `tools/relay/relay.js` — единственный
тестовый control relay. `tools/simturns-relay` — локальный эталон координатора ОХ,
используемый Local-приёмкой. Игровые правила ОХ общие с лобби; локальный pipe-адаптер
доступен только в сборке с харнесом. Без него `D2MSS_SIMTURNS*` не включает ОХ.

Исторический campaign из 18 зелёных прогонов на исходной DLL не доказывает работу
новой DLL. Offline PASS, отдельный canonical PASS и полный campaign PASS — разные
границы. Результат каждого нового кандидата определяется его собственными артефактами.

## Проверки без игры

Из корня репозитория, с Node.js 20+, PowerShell 7+ и C++17-компилятором:

```powershell
node --test tools/relay/test/relay-v2.test.js
node --test tools/test/battle-trace-audit.test.js
./tools/test/simturns-battle-cases-test.ps1
Push-Location tools/simturns-relay
node --test
Pop-Location
./tools/test/simturns-production-poc.ps1 -StaticCheck
./tools/test/auto-battle-http-contract.ps1
./tools/test/literal-inner-startup-test.ps1
./tools/test/legacy-mass-oracle-test.ps1
./tools/test/simturns-acceptance-campaign.ps1 -StaticCheck -GameDir . `
  -FixtureManifest ./tools/test/fixtures/simturns-russobit.json `
  -ArtifactDir (Join-Path $env:TEMP ('d2-oh-static-' + [guid]::NewGuid().ToString('N')))
```

Portable client transcripts компилируются из четырёх исходников:

```powershell
$testExe = Join-Path $env:TEMP ('d2-oh-core-' + [guid]::NewGuid().ToString('N') + '.exe')
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Imss32/include mss32/src/simturns/protocol.cpp mss32/src/simturns/control_client_core.cpp mss32/src/simturns/turn_context.cpp mss32/tests/simturns_control_client_core_test.cpp -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed' }
& $testExe
```

Эти команды не запускают игру. CI выполняет их отдельными contract jobs наряду
с существующими pipe/TCP smoke, проверками шаблонов и обычными gameplay jobs.
Числа 15 C++ и 161 production-JS описывают перенесённый набор, а не заменяют
реальный итог тестового процесса. `prove-russobit-ui-bind-sites.py` требует точный
локальный EXE и не входит в обычную проверку без игры.

## Полная живая приёмка

Нужны точный Russobit `Discipl2.exe` и карта `Exports/DevouringMarshes_Eng.sg`,
соответствующие размерам/SHA-256 в [manifest](fixtures/simturns-russobit.json),
включённый DirectPlay и DLL текущего кандидата с `D2_TESTDRV` (обычно DebugTest).
Runner проверяет fixture и запускает два настоящих клиента. Он не собирает,
не подменяет и не восстанавливает DLL/EXE/карту; подготовка кандидата выполняется
отдельно. Не направляйте тест на обычную игровую установку без её резервной копии.

Данные подготовки и маршрутов находятся в
[`devouring-reinforcement.ini`](fixtures/devouring-reinforcement.ini), не в native-коде.
Runner передаёт абсолютный `D2TESTDRV_FIXTURE_PLAN` обоим клиентам; файл читается
и проверяется один раз. Только `D2TESTDRV_APPLY_FIXTURE=1` на хосте разрешает
однократную подготовку групп на первом server turn-zero. Один лишь путь к плану
ничего не перемещает. Не включайте APPLY вручную для режимов, где runner не
запрашивает подготовку; сохраняйте SHA-256 выбранного INI вместе с результатом.

Из корня репозитория укажите подготовленную тестовую установку:

```powershell
$gameDir = (Resolve-Path (Read-Host 'Папка подготовленной Russobit-тестовой игры')).Path
./tools/test/simturns-acceptance-campaign.ps1 -GameDir $gameDir `
  -FixtureManifest ./tools/test/fixtures/simturns-russobit.json `
  -ArtifactDir (Join-Path $env:TEMP ('d2-oh-campaign-' + [guid]::NewGuid().ToString('N')))
```

План по умолчанию фиксирован: **5 canonical + 1 battle-block + 12 ordered = 18**
независимых холодных сессий. `-Count N` меняет только число canonical, поэтому
общий размер плана равен `N + 13`. Ordered-прогоны чередуют `join-first` и
`host-first`; каждый начинает свежую карту и включает свою literal Phase C.
Они не продолжают ранее завершённый canonical.

Campaign проверяет не только exit code, но и дочерние `summary.json`, topology,
merge, gameplay и owned-process evidence. Не превращайте исключение, отсутствующий
summary или незавершённый дочерний прогон в PASS. Каждому запуску нужен новый
`ArtifactDir`; `-Keep` относится только к одиночному POC и оставляет его процессы
для ручного осмотра, поэтому не подходит для холодной кампании.

## Отдельные проверки боя и умершего лидера

Они **не заменяют и не изменяют** 18 сценариев кампании. Нужен текущий DebugTest
кандидат; OBS/видео не используются. Офлайн-тесты новых helpers проверяют их
контракты на моделях внешних действий, а не выдают результат живой игры.

| `-BattleCase` | Что выполняется | Граница результата |
|---|---|---|
| `none` (по умолчанию) | Исходный `battle-block` №6 | Его прежние проверки, в том числе точные MP, сохранены |
| `audit-idle` | Один бой host, второй клиент не двигается | HP лидера после боя и структурная проверка трассы |
| `audit-moving` | Такой же бой host, четыре шага join во время этого боя | Те же наблюдения плюс непрерывность боя при движении peer |
| `dead-leader` | Отдельная подготовка host с HP0 у лидера; два шага во время боя join, завершение боя и новый день | Два списания по 6 MP, синхронный новый день и восстановление MP35; не исчерпание MP |

Для каждого запуска создавайте новый каталог артефактов. После подготовки `$gameDir`
как выше, из корня репозитория:

```powershell
$case = 'audit-idle' # audit-moving или dead-leader
./tools/test/simturns-production-poc.ps1 -GameDir $gameDir `
  -FixtureManifest ./tools/test/fixtures/simturns-russobit.json `
  -GameplayMode battle-block -BattleCase $case -MergeDay 3 -BarrierOrder parallel `
  -ArtifactDir (Join-Path $env:TEMP ('d2-oh-' + $case + '-' + [guid]::NewGuid().ToString('N')))
```

`audit-*` включает `D2TESTDRV_BATTLE_TRACE=1`; отдельно доступен `-BattleTrace`.
Наблюдатель под `D2_TESTDRV` пишет сырые очереди, границы вызовов, выбранные AI
действия и HP. Он не меняет RNG, AI, урон или правила ОХ. После завершения обоих
принадлежащих прогону процессов runner разбирает сохранённые логи и добавляет
ссылку на `battle-trace-audit.json` в summary. Неполная запись, разрыв sequence,
несовпавшая пара вызовов или лимит записей дают ненулевой exit code. `-Keep`
с трассировкой запрещён: финальные логи ещё не получены.

`auditDeployment` и `postBattleWorlds` сохраняют реальные ID, владельцев,
координаты, MP и world sequence обоих героев от обоих клиентов. Каждый шаг
`audit-moving` дополнительно требует свежих локального и удалённого подтверждений
точных координат/MP, а не только успешного ответа команды. Эти снимки **не доказывают**,
что спрайт, курсор и обычное выделение мышью обновились: MoveStack адресует отряд по ID.
Хеши загруженных helpers/parser сохраняются в `battleCaseSources`; изменение этих
исходников во время запуска запрещает PASS.

Смерть лидера в `audit-*` — наблюдаемый исход, не автоматическая ошибка. Выбор
AI-действия не равен выполненному удару; промахи и призыв могут не иметь записи
`damage-hit`. `complete=true` доказывает только структурную целостность переданных
наблюдений, **не отсутствие пропущенных ударов движка**. Трасса не имеет конечного
native watermark, поэтому удаление целого хвоста завершённых пар сам parser не
обнаруживает; проверка происхождения завершённых логов выполняется runner отдельно.

`dead-leader` использует [отдельный профиль](fixtures/devouring-dead-leader.json).
Runner сохраняет базовый INI и профиль в каталоге прогона, добавляет в рабочую копию
плана `healthCount=1` / `[health1]`. Native проверяет точный unit/type/HP и вызывает
штатный `VisitorApi::changeUnitHp`, который обновляет `leaderAlive`. Базовый INI,
карта и подготовка прежних 18 тестов не меняются. Этот искусственно заданный
предстартовый HP0 проверяет последствия смерти, **не объясняет естественную смерть
лидера в бою**. Evidence: `fixturePlan`, `battleBlock` и, для `audit-*`, `battleTrace`
в summary; путь проверки: профиль → native visitor → состояния обоих клиентов →
отдельный сценарный verdict.

Только новый `dead-leader` привязывает также выходы из гарнизонов к исходным MP35:
это отличает поздний возврат на клетку выхода с MP29 от повторного выполнения выхода.
Legacy deploy/attack и native-защита от повторов остаются прежними.

## Все одиночные режимы

Один сценарий запускается тем же runner; например:

```powershell
./tools/test/simturns-production-poc.ps1 -GameDir $gameDir `
  -GameplayMode canonical -FixtureManifest ./tools/test/fixtures/simturns-russobit.json `
  -ArtifactDir (Join-Path $env:TEMP ('d2-oh-single-' + [guid]::NewGuid().ToString('N')))
```

| `-GameplayMode` | Что проверяет | Входит в 18 |
|---|---|---|
| `protocol` (по умолчанию) | Bootstrap и независимые ходы; с `-MergeDay N` также barrier/merge | Нет |
| `canonical` | Полная закреплённая последовательность боя/движения/ходов до canonical boundary | 5 раз |
| `battle-block` | Движение одного игрока при живом бою другого, включая обратную сторону сценария | 1 раз |
| `ordered-masstest` | Свежая карта, четыре отдельных End Turn intent, ordered merge и literal Phase C | 12 раз |
| `long-move` | Выбранный `-LongMoveCase` из таблицы ниже | Нет |
| `long-attack` | Отдельный длинный attack/charge/convergence сценарий | Нет |

Для `ordered-masstest` обязательны `-BarrierOrder host-first` либо `join-first`,
`-PostMergeContinuationMode automatic-masstest-phase-c-literal` и merge day 3
(можно оставить 0: runner возьмёт 3 из manifest). Campaign задаёт эти параметры сам.
Остальные fixture-based режимы также сверяют merge day с manifest.

### Пять вариантов long-move

В одиночном runner используйте `-GameplayMode long-move -LongMoveCase clean-long-concurrency`;
для другого варианта замените значение `-LongMoveCase` на имя из таблицы.

| `-LongMoveCase` | Проверка | Допустимый вывод из зелёного результата |
|---|---|---|
| `source-route-control` | Исторический маршрут join при idle host | Диагностика воспроизведена; `acceptanceClaim=false` |
| `source-pair-repro` | Два исходных исторических запроса | Диагностика воспроизведена; `acceptanceClaim=false`, не доказательство одновременного движения |
| `clean-host-route-control` | Один точный чистый маршрут host, peer не действует | Чистый маршрут host, не concurrency |
| `clean-join-route-control` | Один точный чистый маршрут join, peer не действует | Чистый маршрут join, не concurrency |
| `clean-long-concurrency` (по умолчанию) | Два чистых маршрута, busy-edge completion, MP, пересечение busy-интервалов и сходимость | Положительная concurrency-приёмка |

Перед положительным `clean-long-concurrency` выполните оба clean route control
на том же кандидате/fixture. Два source-варианта остаются диагностическими даже
при зелёном exit code; их нельзя засчитать вместо положительной приёмки.

### Потеря координатора и продолжение после merge

| Параметры | Назначение и ограничения |
|---|---|
| `-GameplayMode protocol -ProbeRelayFailure -MergeDay 0` | Намеренно останавливает только собственный production-координатор текущего запуска и проверяет fail-closed у обоих клиентов; отдельный destructive-to-this-run probe |
| `-PostMergeContinuationMode none` | По умолчанию: закончить на границе выбранного сценария |
| `-GameplayMode ordered-masstest -PostMergeContinuationMode automatic-masstest-phase-c-literal` | Только literal ordered Phase C; не добавляется к canonical/battle-block |
| `-GameplayMode protocol -MergeDay 3 -PostMergeContinuationMode mss-stock-telemetry` | Отдельная MSS-native проверка stock progression после merge; не утверждает воспроизведение старых таймингов Phase C |

`-IndependentRounds` действует при отключённом merge в protocol-режиме;
`-BarrierOrder` выбирает parallel/host-first/join-first прибытие к barrier.
`canonical`, `battle-block`, `long-move` и `long-attack` не принимают post-merge
continuation. Failure probe несовместим с включённым merge и fixture-gameplay режимами.

## Что сохранять и что считать доказательством

Сохраняйте SHA кандидата/DLL, параметры, manifest, summary и логи каждого падения.
Исторический результат относится только к своей DLL; новая сборка требует нового
запуска. Видео — резервное визуальное свидетельство: оно не заменяет assertions;
отсутствующая запись или single-pane capture не доказывают видимость обоих клиентов.
Не добавляйте ссылки на успешные резервные видео в зелёные summary.

Цепочка проверки: точные DLL/EXE/map и runner arguments → native/UI/world evidence
конкретного дочернего запуска → его summary → строгий campaign verdict.
Публичный lobby, credentials и развёртывание сервера в эту локальную приёмку не входят.
