# Штатный выбор расы и лорда подготовленного матча

Статически подтверждён Russobit x86; игровые проверки ещё нужны. Для других EXE
автоматическая подготовка закрыта проверкой поддержки native API. Нет новых
адресов функции выбора лорда: используется уже созданный callback кнопки игры.

## Область и инструменты

[Область проверки](2026-09-20_prepared-native-scope.md). Цель — доказать выбор первой
расы при свежем создании комнаты и переключение типа лорда без неверного портрета.
SHA256 EXE: `1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb`.

Использованы ida-reverse и docs-generator. Штатный IDA helper вернул `ERR:timeout`;
локальный MCP daemon отсутствовал. Без установки инструментов использован уже
доступный GNU objdump. Патчей бинарника и динамического вмешательства не было.

## Evidence

Все команды выполняются в PowerShell; путь указывает только на разрешённую
тестовую установку. Для другой копии сначала сверить SHA256.

```powershell
$preparedExe = 'C:\GOG Games\slasher_mns_2_4 - Copy\Discipl2.exe'
$preparedObjdump = 'C:\msys64\usr\bin\objdump.exe'
Get-FileHash -LiteralPath $preparedExe -Algorithm SHA256
& $preparedObjdump -d -M intel --start-address=0x4e250e --stop-address=0x4e2607 $preparedExe
& $preparedObjdump -d -M intel --start-address=0x5735a2 --stop-address=0x5735a9 $preparedExe
& $preparedObjdump -d -M intel --start-address=0x42c485 --stop-address=0x42c504 $preparedExe
& $preparedObjdump -d -M intel --start-address=0x55ef42 --stop-address=0x55ef4b $preparedExe
& $preparedObjdump -d -M intel --start-address=0x4e35de --stop-address=0x4e36fd $preparedExe
& $preparedObjdump -d -M intel --start-address=0x4e1f9e --stop-address=0x4e1fd3 $preparedExe
& $preparedObjdump -s --start-address=0x7a3180 --stop-address=0x7a3190 $preparedExe
& $preparedObjdump -s --start-address=0x6dd80c --stop-address=0x6dd820 $preparedExe
```

| ID | Источник / точное наблюдение | Hash |
| --- | --- | --- |
| E-001 | CMenuLobby ctor `0x4e2534..0x4e2546`: функция `0x5735a2` возвращает `CMenuPhaseData+0x28`, список races; `0x42c485` копирует список | EXE SHA256 выше |
| E-002 | `0x4e254b` вызывает `0x55ef42`: первый узел списка, значение с `+8`; `0x4e2557..0x4e2583` переносит категорию в current/pending race, `0x4e2586..0x4e25a4` создаёт ReqRace через `0x47ce79` и отправляет штатно | EXE SHA256 выше |
| E-003 | `0x4e1f9e..0x4e1fce`: BTN_LORD (`0x7a3180 → 0x6dd80c`) привязывает callback `0x4e35de` | EXE SHA256 выше |
| E-004 | `0x4e35de`: циклический индекс типа лорда; `0x4e3642..0x4e3649` создаёт ReqLord с текущим face из `LobbyData+0x20`; отправка `0x4e3665`, обновление изображений `0x4e3686` | EXE SHA256 выше |

## Findings

### F-001 — первая раса задаёт начальный выбор хоста

- Severity: n/a_re; category: reverse_algo; status: validated; confidence: high.
- Evidence: E-001, E-002; location: `CMenuLobby` fresh branch `0x4e250e..0x4e2607`.
- Для свежей сетевой генерации игра сама отправляет ReqRace с первой расой списка
  `CMenuPhaseData.races`. Состав рас для Lua не следует подменять после генерации:
  можно менять только порядок native списка непосредственно перед созданием хоста.
- Ограничение: это не доказательство аналогичного поведения при загрузке сейва.

### F-002 — штатная кнопка лорда сохраняет портрет

- Severity: n/a_re; category: reverse_algo; status: validated; confidence: high.
- Evidence: E-003, E-004; location: BTN_LORD callback `0x4e35de`.
- Достаточно вызвать существующий enabled functor BTN_LORD на конкретном
  `CMenuLobbyHost`, затем запросить и сверить AnsStartInfo. Категории native:
  mage=0, warrior=1, thief=2. Callback сохраняет существующий корректный face.
- Нельзя обращаться к предполагаемым private offsets LobbyData из C++ реализации.

## Path

P-001, callflow: согласие → cached recipe → штатные Retry/Accept → сериализация →
список races с согласованной расой хоста первой (E-001/F-001) → createServer →
native ReqRace (E-002/F-001) → ReqStartInfo → при необходимости BTN_LORD functor
(E-003/E-004/F-002) → проверка AnsStartInfo. После успешного выбора повторных
навязанных переключений не выполняется.

Остаточный риск: статическое доказательство не заменяет два клиента с
`DisplayErrors=1`: первый вход, возврат, Retry, Cancel, 111, нужный lord и портрет.

## Хронология

2026-09-20: проверены исходники и существующие native API; штатный IDA bootstrap
вернул timeout; проверены SHA256 и узкие objdump диапазоны; оформлена доказательная
цепочка до интеграции и игрового теста.

## Проверка реализации — 2026-09-20

Рабочая ветка `codex/prepared-matches` от `47a17f8e`; PR7 не изменён.
MSS Release Win32/v143 собирается штатным проектом; данные конкретного deploy
и SHA установленной DLL фиксируются отдельно от доказательства native-пути.

Консольные проверки прошли:

- `run-prepared-match-protocol.ps1`: wire golden vectors, все усечения, bounds,
  категории native, defaults, immutable snapshot, раса хоста и custom spins.
- `run-scenario-template-recipe.ps1`: свежий Lua/getContents для каждого seed,
  сохранение принятых настроек; оба локальных Outrunner разобраны.
- Независимый cross-wire harness core: настоящий encoder Offer/Cancel прочитан
  настоящим MSS decoder, все MSS status0..8 прочитаны core; усечения отвергнуты.
- `git diff --check` — без ошибок whitespace.

Подтверждение условий использует существующий native Yes/No; объём каждой страницы
измеряется `IFormattedText::getTextHeight` по реальному `TXT_INFO` модового диалога.
Ни одного нового адреса native функции или capability в реализации нет. Категории
рас сверены с `categoryids.h`: Empire0/Undead1/Legions2/Clans3/Elves5; порядок картинок
UI не является значением категории.

Статическое доказательство и компиляция не являются runtime-приёмкой. Обязательный
остаток: первый вход/возврат, длинные условия, Yes/No, отсутствующий/другой Lua,
Retry/Cancel, Cancel одновременно с Accept, native race/lord/портрет, joiner и `111`
на двух клиентах с `DisplayErrors=1`. Сбой native createServer после уже созданной
серверной комнаты требует отдельной runtime-проверки; отмена сайта не должна
прерывать такую комнату или терять её identity.
