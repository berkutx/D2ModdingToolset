# Один харнес, два транспорта ОХ

Харнес управляет настоящими клиентами через один `tools/relay/relay.js` и один
набор команд/наблюдений `D2_TESTDRV`. Он не является реализацией одновременных ходов.
Production-ОХ остаётся тем же кодом PR10; выбирается только источник координатора.

Для ошибки `Simultaneous turns stopped` до старта карты см.
[диагностику первой остановки ОХ](SIMTURNS_DIAGNOSTICS.md).

## Сборка

`/p:D2TestDrv=true` включает харнес независимо от конфигурации.
`DebugTest` — alias Debug с включённым харнесом; отдельного `ReleaseTest` нет.
ОХ включается независимо: `/p:EnableSimultaneousTurns=true`. Для ОХ-тестов нужны
оба свойства; шаблоны и общие UI-тесты не требуют ОХ. Обычная DLL без `D2TestDrv`
не содержит управления тестами и локального pipe-адаптера.

## Выбрать проверку

Команды ниже запускаются из корня worktree с PowerShell 7 и Node.js 20+.
Вместо неявной установки runner требует подготовленную игру и свежий каталог
артефактов. Он не подменяет DLL, EXE, Lua или карты.

| Транспорт | Координатор | Проверка |
| --- | --- | --- |
| `Local` | Собственный `tools/simturns-relay`, test-only pipe adapter | Прежние 18 холодных прогонов и все дополнительные сценарии |
| `Lobby` | Авторизованное production-лобби, штатный Arm/ID+18 | Новая генерация, host/join, оба клиента на карте и production bootstrap release |

### Прежние 18 и дополнительные сценарии

```powershell
$game = (Resolve-Path (Read-Host 'Подготовленная тестовая игра')).Path
$artifacts = Join-Path $env:TEMP ('oh-' + [guid]::NewGuid().ToString('N'))
./tools/test/simturns-test.ps1 -Transport Local -Campaign -StopOnFailure `
  -GameDir $game -FixtureManifest ./tools/test/fixtures/simturns-russobit.json `
  -ArtifactDir $artifacts
```

План сохранён: 5 canonical + 1 battle-block + 12 ordered-masstest. Таймауты,
координаты, MP, причинные подтверждения и критерии исходных сценариев не ослаблены.
Дополнительные `long-move` (5 вариантов), `long-attack`, независимые несколько дней,
`audit-idle`, `audit-moving`, `dead-leader` описаны в [SIMTURNS.md](../tools/test/SIMTURNS.md).
Для одиночного запуска передайте `-GameplayMode`; остальные прежние параметры
передаются без преобразования через `-LocalOptions @{ MergeDay = 3; BattleCase = 'audit-moving' }`.

Только два исторических диагностических `source-*` режима выставляют
`D2TESTDRV_LEGACY_HOST_LOOPBACK=1`. Геометрия `EXACT_LEGACY_MOVES` сохранена и в
canonical, но обычная приёмка не подменяет доставку хоста синхронным вызовом сервера.

Нужны точные EXE и `Exports/DevouringMarshes_Eng.sg` из manifest, совместимые базы
игры и DirectPlay. Наличие одного правильного EXE не делает другую установку
эквивалентной: фиксированные ID/HP/MP зависят также от игровых данных.

### Настоящее лобби

Сначала отдельно подготовьте разрешённую тестовую игру: `[Disciple] DisplayErrors=1`,
явная видимость `settings.lobby.controls.simultaneousTurns=true`, casual defaults,
существующий локальный Lua-шаблон и адрес обновлённого сервера. Runner проверяет
буквальный локальный `controls`-блок; динамический Lua он не исполняет и не изменяет.
Потребуются две разные учётные записи, заданные в окружении процесса:
`D2_LOBBY_HOST_ACCOUNT`, `D2_LOBBY_HOST_PASSWORD`, `D2_LOBBY_JOIN_ACCOUNT`,
`D2_LOBBY_JOIN_PASSWORD`. Значения не передаются аргументами запуска или в URL,
не наследуются игровыми процессами и не записываются в summary.

```powershell
$game = (Resolve-Path (Read-Host 'Тестовая игра с настроенным лобби')).Path
$template = Read-Host 'Точное имя локального Lua без расширения'
$artifacts = Join-Path $env:TEMP ('oh-lobby-' + [guid]::NewGuid().ToString('N'))
./tools/test/simturns-test.ps1 -Transport Lobby -GameDir $game `
  -TemplateName $template -ArtifactDir $artifacts
```

Это явный live-тест: он создаёт одну уникально названную casual-комнату. Join
выбирает точное имя, а native-проверка повторяет соответствие перед callback.
Вход, генерация и старт выполняются штатными UI-командами, без локального ОХ-рилея.
Управление/наблюдение остаётся тем же API. Процессы закрываются только по сохранённым
собственным Process handles; `-Keep` оставляет их для ручного осмотра.

`passed=true` здесь означает **generated-map-bootstrap**, а не 18/18 и не проверку
боёв/нескольких ходов. `-Transport Lobby -Campaign` и fixture-режимы заранее
отклоняются: опубликованный сервер принимает новую сгенерированную карту,
а прежняя кампания требует точную экспортированную карту. Подменять её случайной
картой, имитировать серверные события или объявлять старый результат новым нельзя.

## Проверки без игры

Из MSVC x86 developer shell: `./tests/run-item-potion-fields.ps1 -OutputDirectory ./artifacts/potion-fields`.
Проверяет настоящий загрузчик полей зелий с подставленными DB API в Debug и Release.
Пустой `MOD_POTION` (включая DBF padding) означает отсутствие модификатора;
непустое значение остаётся под строгой проверкой игры. Причина прежнего окна:
`readPotionExtraFields` вызывал `readId` для пустой строки, а Debug-перехватчик
показывал исключение ещё до `catch`. `DisplayErrors` и debug mode не отключаются.
Эта проверка не заменяет запуск клиента с настоящей базой.

```powershell
./tools/test/lobby-simturns-smoke.ps1 -StaticCheck
node --test tools/relay/test/relay-v2.test.js
./tools/test/simturns-battle-cases-test.ps1
./tools/test/legacy-mass-oracle-test.ps1
```

OBS не включается и видео не требуется. Сохраняются summary и собственные журналы;
успешная компиляция/модели не заменяют живой прогон двух клиентов.

Основание переноса: donor `b7637a03`, его `simturns-acceptance-campaign.ps1`,
`simturns-production-poc.ps1` и manifests. Путь доказательства прежний:
конкретная DLL/EXE/fixture → native/UI/world observations → сценарный summary →
campaign verdict. Для нового lobby smoke граница результата указана отдельно.
