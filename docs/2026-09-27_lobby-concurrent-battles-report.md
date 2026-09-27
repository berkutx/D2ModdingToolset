# Реальные одновременные бои через лобби

Проверка выполнялась двумя настоящими клиентами через native testdrv, без
computer-use и без ручных команд между шагами. Новый сценарий проверяет два
раздельных боя против нейтралов и объединение на дне 3; это не старая 18-case кампания.

## Результат

Run006 прошёл: два нативных боя B, положительное пересечение интервалов 19 390 мс,
совпавшие исходы в обоих мирах и объединение на дне 3. У обоих клиентов одна
транзакция `actionId=6` и естественный BeginTurn `sequence=57`, затем drained и
Stock release. `concurrentBattleMergeAcceptance=true`, `fullGameplayAcceptance=false`.
Ни Lua-ошибок, ни terminal fault; настройки не изменились, `cleanupErrors=[]`.
Подготовка закрыта без результата и PTS; оба собственных процесса завершены.

Контрольный run007 на итоговой DLL также PASS: 21 281 мс пересечения,
merge day3 / action6 / общий sequence50. Те же проверки отсутствия Lua/OH faults,
неизменности файлов и очистки прошли. Пересечение означает интервалы нативного
callback автобоя → наблюдение результата; это не замер длительности отдельных
анимаций или независимый серверный ledger.

## Область изменений

Все изменения C++ и тестового runner относятся к harness-only PR11.
Production PR10, сервер, сайт и их таймауты в этой работе не менялись.
Игра запускалась с `DisplayErrors=1`; подготовки нерейтинговые и принадлежат тесту.
Очистка закрывает только собственные процессы и подготовку, не назначая результат/PTS.

Новый runner использует существующие native actors. Добавлены наблюдаемый выход
из столицы 5×5, обе реальные раскладки боя A/B и точное окно итогов хода
`DLG_TURNSUMMARY::BTN_OK` в lobby-режиме. Проверки владельца, возраста binding,
типов callback и однократности действий сохранены. Запросы после неоднозначного
ответа не повторяются; неизвестные окна не закрываются.

## Evidence

Все локальные артефакты ниже находятся в `artifacts/`, не входят в публичный Git.
Времена — UTC, дата 2026-09-27. Внешних учётных данных в отчёте нет.

| ID | Время / источник | Наблюдение | SHA-256 |
|---|---|---|---|
| E-01 | 05:35:55.863; `oh-lobby-battles-day3-20260927-002/host.mss32.log` | `getModifierDisplay`: expected boolean, received no value | `76e4f5ba6a7818815975adfddd312108108417f180a4b8bb56894f1ecbdac611` |
| E-02 | Lua-файл до изменения; `lua-display-fallback-20260927/z_unit_effect.original.lua` | Три display-функции не имели return на пути без совпавших условий | `79dcf05dd7ffa99e084f4d4b8dc6662108caf13487aebd99cf3d919435a7d85e` |
| E-03 | Lua-файл после разрешённого изменения; `lua-display-fallback-20260927/z_unit_effect.lua` | Только три строки `return prev`; 25 проверок реальных Lua-функций PASS, исходник RED | `7e41de991eed1002eae30833c244a6c548684ee5f0526046152a56c8eca375b1` |
| E-04 | 05:56–05:57; `oh-lobby-battles-day3-20260927-003/summary.json` | Startup PASS, без Lua/OH fault; проверку прервал PowerShell binding пустых строк | `42e56f195efec4cba28443dd5a21765bb20bf990ebeb5cd679fa2a4c3c579198` |
| E-05 | 06:26–06:27; `oh-lobby-battles-day3-20260927-006/summary.json` | Полный ограниченный сценарий PASS, обе роли Stock, чистая очистка | `c555cc9e974ede4250e9096494729de31906de024ae126686581ca6e748297f6` |
| E-06 | run006; `concurrent-native-battle-overlap.json` | Общий monotonic clock, 19 390 мс пересечения exact-identity боёв | `f696087e07840a7464120a6db7950f9645e248303aff0daea1dbd2b7ab7213f1` |
| E-07 | run006; `concurrent-battle-merge-proof.json` | Бои, исходы, два EndTurn и native merge day3 | `8c36b8db064d6ac150655fe391be4156073c3a2c95cbbb0d8139d0017fd08429` |
| E-08 | 06:33; `oh-lobby-battles-day3-20260927-007/summary.json` | Повторный PASS на итоговой DLL, cleanupErrors пуст | `a37223c29dccc64e426a313f322520e031c3a536c92774d824cfd9cf8827b9c6` |
| E-09 | run007; `concurrent-native-battle-overlap.json` | 21 281 мс на общем monotonic clock | `6c787184d955b68ba94e7a6fdf9da0a1ebcce70d848cc6db61d4ebad32d90749` |
| E-10 | run007; `concurrent-battle-merge-proof.json` | Общий sequence50, merge day3, Stock обеих сторон | `683c94c5bdaacf846165ec682a6a50a20f0652e0a8038b5963d1b0ec70863a2d` |

Воспроизведение E-01–E-04 ограничено наличием сохранённых локальных артефактов.
`lua-display-fallback-20260927/check.lua` загружает реальные функции, подставляет
только условия и зависимости отображения, проверяет fallback и все семь прежних
веток. Боевые формулы не изменены и этим тестом не проверяются.

Для чтения завершённого run003 без игровых процессов и без изменения журналов:

```powershell
./tests/run-simturns-lobby-gameplay-run.ps1 -ReplayArtifactDirectory ./artifacts/oh-lobby-battles-day3-20260927-003
```

Оба точных native автобоя и закрытия результатов проходят replay. Пустые строки
теперь разрешены в массивах логов; пустой лог по-прежнему не доказывает успех.

## Findings

- F-01, validated, confidence high, E-01–E-03: ошибка принадлежала модовому Lua,
  а не протоколу ОХ. Исправление установлено только после отдельного разрешения,
  с побайтно проверенным резервом. MSS не стала молча принимать `nil` вместо boolean.
- F-02, validated, confidence high, E-04 и replay: старая автоматизация распознавала
  только `DLG_BATTLE_A`, а СМНС открыла B. Теперь layout закрепляется вместе с owner
  и appearance; A/B не разрешают подмену identity внутри боя. Реальный B callback
  прошёл прежние точные проверки native layout и postcondition.
- F-03, validated, confidence high, run004: оба боя завершены, их исходы совпали
  в двух мирах (`battle-world-outcomes.json`, оба героя проиграли нейтралам).
  Первый парный EndTurn прошёл, но хост остался на окне итогов. Сервер сообщил
  `ordinary-activate actionId=2 timed out` в 06:03:10.852, room18/epoch17.
  В клиенте после открытия окна оставалось `pendingLocalUpdates=1`.
  В run004 объединение не состоялось. Добавленная обработка окна — изменение
  тестового драйвера, не ослабление production-таймаутов.
- F-04, validated, confidence high, run005/E-05: после боя корень карты мог быть
  `DLG_ISO_PAL`, а кэшированная привязка стратегической панели указывала на уже
  закрытую модалку. Теперь её присутствие проверяется по настоящей цепочке родителей
  с обратной проверкой каждого ребра. Чужой owner не подставляется. Глубина, число
  детей и изменения индекса getter ограничены; fault/cycle прекращают наблюдение.
- F-05, validated, confidence high, E-05–E-07: в run006 точные TurnSummary callback
  прошли один раз у обеих сторон, за ними наблюдался `ActivateTurn UI queue drained`
  (host action2 / join action3, день2). Далее день3/Stock прошли без таймаута.
  Это live-подтверждение исправленного сценария, не только mock-тест окна.

## Path проверки

1. Собственная casual-подготовка с закреплённым merge day → native Login,
   Generate/Accept/Join/Start → startup receipts и bootstrap (E-04).
2. Свежие согласованные миры → проверенные native выходы из столиц → два разных
   нейтрала → две однократные атаки. Расстояние не выдаётся за проходимость маршрута.
3. Точные auto/close receipts задают нативные интервалы callback автобоя →
   наблюдение результата на общем `GetTickCount64`. Их положительное пересечение
   обязательно; сохранённый UI связывает identity, но не доказывает длительность.
   Затем проверяются совпавшие исходы в обоих мирах (F-02/F-03).
4. Два парных EndTurn → native prepare/execute, естественный BeginTurn с общим sequence,
   drained и Stock release с общим actionId. Без всей цепочки
   `concurrentBattleMergeAcceptance` остаётся false.

Команда полного сценария после настройки окружения из [инструкции](TEST_HARNESS.md):

```powershell
./tools/test/simturns-lobby-e2e.ps1 -ArtifactDir ./artifacts/lobby-battles-day3 -ExpectedMergeDay 3 -Gameplay ConcurrentBattles
```

Каталог должен быть новым. Runner не обновляет Lua, не меняет игровые настройки,
не подменяет карту и не расширяет права тестовых аккаунтов. Проверка клиентских
маркеров не является независимой проверкой полного серверного transaction ledger.

## Сборка и регрессии

Native source `411e4355a071703d31ccf5ce2a1e5ae6d05fee43`, DebugTest Win32,
OH и testdrv включены. Итоговая DLL установлена с проверенным резервом:
SHA-256 `43c2f8155262b21c1ad2e5271a2587852955609b3e8c0e338ab93c14348502e6`.
Сборка: 0 ошибок, прежнее предупреждение LNK4075. Это DLL контрольного run007;
run006 использовал `5b078790249312a36fdc6b1fba9b6d07d2b37fed9c5d2f9dd4fecac3aa711f3a`.
Различие — дополнительная защита обхода дерева UI от изменения индекса native getter.

Пройдены локально: 109 relay-тестов, 21 API-тест подготовки, 135 проверок E2E,
112 проверок gameplay policy, 381 проверка runner (392 с replay run003/006/007).
Actual-function native tests:
35 capital-exit, 7 UI-reveal, 15 ancestry; popup state machine PASS. Старые
battle-case 34/34, battle-trace 17/17, mass oracle и literal startup PASS.
Проверки платформенных API в этих unit-тестах подставлены; реальное выполнение
доказано отдельно E-05–E-10. Удалённый CI этим списком не утверждается.

После run007 изменена только защита владения каталогом артефактов в PowerShell:
атомарный claim до запуска/учётных данных, проверка всех предков и запрет cleanup
чужой подготовки при коллизии. Реальные временные collision/junction-регрессии
входят в 135 offline-проверок; повторный live для этой финальной PS-правки не заявлен.
