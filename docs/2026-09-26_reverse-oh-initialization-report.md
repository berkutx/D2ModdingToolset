# ОХ: джойнер в меню при запуске хоста

Исправление относится к production PR №10, без игрового тест-драйвера.
Подтверждение имени **по умолчанию** тоже отправляет сообщение изменения имени:
ручного ввода текста для воспроизведения не требуется. Успешная компиляция и
портативная регрессия не означают пройденную двухклиентную игровую приёмку.

## Границы

Пользователь разрешил диагностику и исправление ОХ, обновление PR10/PR11 и
проверку двух своих тестовых клиентов. Анализируется локальная копия PE32 Russobit
`Discipl2.exe`, 4 187 648 байт, SHA256
`1375cdef09ec470ee64fe5693fb734d7c69fb215212311d997f792b258a642eb`.
Исходный EXE, сценарии и настройки игры не изменяются. Сеть — только согласованный
тестовый lobby и локальный relay тест-драйвера; в публичных материалах нет паролей.
Локальный scope предыдущего анализа: `artifacts/oh-refresh-native/scope.md`.

## Evidence

E-001 — реальный прогон PR11 `artifacts/oh-real-lobby-20260926/run-004`.
`host.mss32.log` SHA256
`197bad6c6944b77f2ee0f845351d8b993c36a0f914dd9c7abfa1d652c727d627`;
`join.mss32.log` SHA256
`c5a51e33d4a362825cc3768adad1dce309dfde518fb9816faa190d88f4565ce1`.
18:18:27.713: драйвер закрывает `DLG_GETINFO_BOX::BTN_CLOSE`, не меняя поле имени.
Хост отправляет `CStackChangeLeaderNameMsg` (59 байт). Native сервер рассылает
`CCmdUpdateObjMsg` (52) и `CRefreshInfo` (118). Джойнер ещё в меню: dispatch
выполнен, handler_count=0, результат Unhandled. Проверка завершения вызывает Abort.
Воспроизведение требует именно двух клиентов и задержки старта джойнера после
подтверждения хостом стартовых диалогов; смена имени не требуется.

E-002 — IDA, локальная disposable database `d2_oh_init_20260926`.
`artifacts/oh-refresh-native/targeted-updateobj-evidence.json`, SHA256
`f497b70ed89ca0fd971d917aa92d7cb39304e4bd4cf2dc337cf4841e5d188b6b`.
Повторный сбор из этой базы: `artifacts/oh-refresh-native/collect-updateobj-evidence.ps1`.
Скрипт проверяет SHA EXE; исходный бинарник не патчится. Публичный PR не содержит
чужого EXE или локальной IDA-базы; адреса и выводы приведены здесь:

- `0x47C0BB` — factory case 1 (UpdateObject), вызов конструктора `0x47B902`.
- `0x47B90A` — vtable `0x6D4F34`; serializer slot `0x47F663`.
- `0x47F66C` — базовый заголовок 44; `0x47F676` — recipient ID 4;
  `0x47F680–0x47F685` — sequence 4. Итого строго 52, без дополнительных полей.
- RTTI `0x792700` подтверждает `CCmdUpdateObjMsg`.
- `0x40FDE7–0x40FDF5` регистрирует callback `0x4102B0` через `0x410F5E`;
  vtable `0x6CEE9C` → COL `0x6F7FE8` → RTTI `0x790250`:
  `CNetMsgMapEntry_member<CCmdUpdateObjMsg, CMidCommandQueue2::CNMMap, ...>`.
- `0x421B49`, ранее разобранный путь: адресату сначала отправляется
  `CNewScenarioMsg`, затем полный снимок объектов. Подробнее:
  [разбор начального снимка](2026-09-24_reverse-oh-refresh-report.md).

E-003 — portable actual-adapter regression. До правки production
`lobby_transport.cpp` SHA256
`3c06c9c67ad50360ed9165865632998173832a1f3e62e3807f2f3def4f95c6a5`.
Debug и Release воспроизводят отказ
`run004 pre-snapshot UpdateObj(52) aborted or fabricated an engine acknowledgement`.
Локальные transcripts: `artifacts/oh-production-startup-run004-red-20260926`.
Тест связывает настоящий lobby adapter, CoordinatorPort и control core; подменены
только внешние границы. Синтетические recipient/sequence `{0,2}` обозначены как
реконструкция: их значения не выдаются за байты, захваченные из run004.

E-004 — read-only сравнение Git: `git show 94ef5e6d -- mss32/include/netcustomservice.h mss32/src/netcustomservice.cpp`
и текущая реализация JoinByFilter/EnterRoom; content_hash=n/a (идентификатор Git).
Этот production-коммит присутствовал в ancestry PR11, но не PR10. Перенесённые
два service-файла побайтно совпадают с PR11 `9eb86dda`; новое API не требуется.

## Findings и исправление

F-001; severity=n/a_re; evidence_ids=E-001,E-002,E-003; confidence=high;
status=validated; location=`lobby_transport.cpp`, `native_notification_policy.h`.
Ранний штатный broadcast попадает клиенту без игровой очереди. Unhandled в этой
конкретной стадии не доказывает потерянное игровое обновление: будущий снимок
передаёт актуальное состояние. Но считать такой пакет Applied или подтверждать
его как выполненную команду также нельзя.

Теперь ticket завершается как Filtered только для точного UpdateObj(52), от
native server к join-клиенту, до собственного NewScenario/StartScenario, в одной
и той же ненулевой pregame-generation без CMidClient до и после dispatch.
Вложенный переход через границу снимка запрещает исключение. Failed/Drop,
неправильные класс/размер/направление и поздний Unhandled остаются ошибками.
Применённые ранее команды продолжают ждать настоящего опустошения очереди;
раннее обновление не разблокирует их fence и не создаёт ACK. Replay в новую
очередь не делается.

F-002; severity=n/a_re; evidence_ids=E-004; confidence=high; status=validated;
location=`netcustomservice.cpp::RoomsCallback::JoinByFilter_Callback`.
PR11 уже содержал production-исправление `94ef5e6d`, отсутствовавшее в PR10:
обычный join использует JoinByFilter, а binding устанавливался только EnterRoom.
В PR10 перенесены только service callback и безопасное чтение необязательных
OH-свойств legacy-комнаты. Подготовленные приглашения, игровой драйвер и
несвязанные изменения PR11 сюда не включаются.

## Path и проверки

P-001; path_type=callflow; evidence=E-001,E-002; finding=F-001.
Подтверждение исходного имени → native rename → UpdateObj + Refresh broadcast →
джойнер без CMidClient → нулевое число обработчиков → ограниченное завершение
ticket → собственный NewScenario и полный снимок → обычные строгие проверки.

Из корня checkout, в MSVC x86 developer environment:

```powershell
./tests/run-simturns.ps1 -OutputDirectory ./artifacts/oh-init-tests
```

После исправления этот общий runner прошёл: пять исходных suites и все 19
actual-adapter cases в Debug и Release. Локальные логи:
`artifacts/oh-production-startup-green-20260926/lobby-startup`.
Он включён в CI с сохранением transcript; игровой `D2_TESTDRV` не нужен.

Timeline: 26 сентября — сохранён реальный отказ run004; подтверждены сериализатор,
владелец обработчика и граница снимка; зафиксирована красная регрессия в двух
конфигурациях; внесены ограниченное исправление и независимый JoinByFilter port.
Итог сборки и следующего живого прогона фиксируется отдельно после выполнения.
