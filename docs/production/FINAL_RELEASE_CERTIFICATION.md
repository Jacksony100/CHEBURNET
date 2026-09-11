# CHEBURNET v1.0.0 Production Certification

Status: **RELEASE_CANDIDATE_READY_WITH_BLOCKERS**

Branch: `release/production-hardening-v1`
SHA: `4b027f7e813abd1476f2784442c04c94da52625b` (база: `78cc980a6d0d68f669cf98eff3799c1cf5fa02bd`, тег `v1.0.0-rc.3`)
Date: 2026-09-11

Этот документ фиксирует состояние **кандидата** `1.0.0-rc.3` с выполненной
подготовкой к стабильному выпуску. Стабильный тег `v1.0.0` ещё не создан:
`cmake/Version.cmake` по-прежнему объявляет `rc.3`, и это верно — переключение
на стабильную версию выполняется одной правкой после закрытия блокеров ниже.

## Launcher

| Поле | Значение |
|---|---|
| Semantic version | `1.0.0-rc.3` |
| Release channel | `prerelease` |
| PE FileVersion / ProductVersion | `1.0.0.3` |
| Authenticode | **не подписан** (сертификат в этом окружении отсутствует; для RC это разрешено политикой) |
| SHA-256 | `d319010e2f32b91095325bc99cd5e742785ae2a0293a07a5e935739bff664cdf` |
| Размер | 4 159 488 байт |

## Payload

| Поле | Значение |
|---|---|
| Provider | `Flowseal/zapret-discord-youtube` |
| Version | `1.10.2` |
| Archive SHA-256 | `5eaac9fb2e4b1abd693487452a3ff3f4dfe9578a45f9ddddfa4bc1f5a6bb62d5` |
| Upstream commit | `dfd8e613b099676cf2aa7b474ee5923801514dec` |
| Release ID | `375784527` |
| Package SHA-256 | `970c8550c3e0498f4a60d5e8900b7e590c7592757975accb5335b126ef463333` |
| Package size | 3 403 061 байт, 27 файлов |
| Стратегий | 22 (было 21; добавлена `ALT13`) |

## Verification

Все команды выполнены на `4b027f7`, Windows 11 Pro 26200, MSVC (VS 18 BuildTools), Ninja, Release.

| Проверка | Команда | Результат |
|---|---|---|
| Clean build | `scripts\build-release.ps1 -BuildDir build-final` | **PASS** — `CHEBURNET.exe` 3.97 MB, предупреждений нет (`/W4 /WX`) |
| CTest | `scripts\run-tests.ps1 -BuildDir build-final` | **PASS — 43/43** |
| Strategy fidelity | (в составе `run-tests.ps1`) | **PASS — 88/88** (22 стратегии × 4 режима GameFilter) |
| Security regression | тест `security_regression` | **PASS** |
| Package safety | тест `package_safety` | **PASS** |
| Release metadata | тест `release_metadata` | **PASS** |
| Release signing | тест `release_signing` | **PASS** |
| Version model | тест `version_model` | **PASS** |
| Upstream automation | тест `upstream_automation` | **PASS** |
| Importer validation tree | тест `importer_validation_tree` | **PASS** |
| E2E harness self-test | тест `e2e_harness` | **PASS** |
| Fuzz / properties | тест `fuzzparsers` | **PASS** (+ кампании 500 000 и 3 × 50 000 итераций на разных зёрнах) |
| Fault injection | тест `faultinjection` | **PASS** |
| Diagnostics privacy | тест `diagnostics` | **PASS** |
| Startup recovery | тест `runtimerecovery` | **PASS** |
| License validation | `scripts\validate-licenses.ps1` | `LICENSE_VALIDATION: PASS` |
| Upstream fidelity | `scripts\verify-upstream.ps1` | `UPSTREAM_FIDELITY: PASS version=1.10.2 files=46 strategies=22` |
| Upstream currency | `scripts\sync-upstream.ps1 -Version 1.10.2 -CheckOnly` | `UPSTREAM_IMPORT: already current (1.10.2)` |
| Static analysis | `cmake -DCHEBURNET_ENABLE_ANALYZE=ON` + сборка | **PASS** — находок в коде CHEBURNET нет, сборка идёт с `/WX` |
| Packaging | `scripts\package.ps1 -BuildDir build-final -OutDir dist-final -SkipBuild` | **PASS** |
| Release metadata generation | `scripts\prepare-release.ps1 -Tag v1.0.0-rc.3 …` | `RELEASE_PREPARATION: PASS tag=v1.0.0-rc.3 launcher=1.0.0-rc.3 pe=1.0.0.3 channel=prerelease payload=1.10.2` |
| Windows 11 clean VM | `scripts\e2e\Invoke-CheburnetE2E.ps1` | **НЕ ВЫПОЛНЕНО** — блокер B1 |
| Windows 10 clean VM | `scripts\e2e\Invoke-CheburnetE2E.ps1` | **НЕ ВЫПОЛНЕНО** — блокер B1 |
| Connect / disconnect / relaunch | ручная часть шлюза | **НЕ ВЫПОЛНЕНО** — блокер B1 |
| Update / rollback на живой системе | ручная часть шлюза | **НЕ ВЫПОЛНЕНО** — блокер B1 |
| Stable Authenticode signature | workflow публикации | **НЕ ВЫПОЛНЕНО** — блокер B2 |

Проверенный выборочно артефакт подписанного манифеста (RC-канал):

```json
{"schema":1,"channel":"stable","key_id":"cheburnet-release-2026",
 "launcher":{"version":"1.0.0-rc.3","sha256":"d319010e…4cdf","size":4159488,
             "minimum_supported_version":"1.0.0-rc.1"},
 "payload":{"provider":"Flowseal/zapret-discord-youtube","version":"1.10.2",
            "sha256":"970c8550…3333","size":3403061,
            "minimum_launcher_version":"1.0.0-rc.1",
            "payload_schema":1,"strategy_schema":1}}
```

Это и есть главное доказательство исправления дефекта версий: до работ
`launcher.version` был бы `1.0.0` и для RC-сборки тоже.

## Remaining known issues

### B1 — проверка в чистом окружении Windows не выполнена

- **Серьёзность:** блокирующая для стабильного выпуска (AC-08).
- **Причина:** привилегированные сценарии изменяют фиксированное защищённое
  расположение `%ProgramData%\CHEBURNET` и требуют загрузки драйвера ядра.
  Выполнять их на рабочей машине владельца репозитория недопустимо; сессия не
  элевирована, машина не помечена как одноразовая, виртуальной машины с
  установленной Windows в окружении нет.
- **Что уже сделано:** оснастка готова и самопроверена, сценарий отказа запуска
  без прав администратора выполнен и пройден, подготовка VM автоматизирована.
- **Точное следующее действие:**
  1. `scripts\e2e\New-CheburnetTestVm.ps1 -Edition win11 -IsoPath <ISO> -ReleaseDir dist`;
  2. установить Windows, сделать снимок `clean-after-setup`;
  3. в госте `.\scripts\e2e\Run-InGuest.ps1`;
  4. повторить для `-Edition win10`;
  5. выполнить ручную часть шлюза из `docs/production/E2E_RELEASE_GATE.md`;
  6. приложить оба `e2e-report.json` к этому документу.

### B2 — подпись Authenticode стабильного выпуска не проверена на практике

- **Серьёзность:** блокирующая для стабильного выпуска (AC-03).
- **Причина:** сертификат подписи кода отсутствует в этом окружении. Политика,
  шлюзы и их отказы проверены полностью (включая реальные подписи и файл,
  изменённый после подписи), но подпись **именно сертификатом CHEBURNET** ни
  разу не выполнялась.
- **Точное следующее действие:** задать секреты `AUTHENTICODE_PFX_B64` и
  `AUTHENTICODE_PFX_PASSWORD`, опубликовать RC-тег и убедиться, что шаг печатает
  `AUTHENTICODE_SIGN: PASS status=Valid timestamped=True`. Только после этого
  создавать стабильный тег.

### B3 — переключение версии на стабильную не выполнено

- **Серьёзность:** обязательное действие перед выпуском, не дефект.
- **Причина:** источник версии намеренно оставлен на `rc.3`, чтобы ветка
  оставалась кандидатом.
- **Точное следующее действие:** в `cmake/Version.cmake` заменить
  `set(CHEBURNET_VERSION_PRERELEASE "rc.3")` на `set(CHEBURNET_VERSION_PRERELEASE "")`,
  пересобрать, убедиться что `version_model` и `release_metadata` проходят, и
  создать тег `v1.0.0`. Шлюз тега не даст опубликовать стабильный тег при
  RC-источнике и наоборот.

### B4 — тесты CodeQL и процесса static-analysis не выполнялись на GitHub

- **Серьёзность:** средняя, не блокирующая.
- **Причина:** процессы добавлены в этой ветке и запускаются на GitHub, здесь
  выполнить их нельзя. Локальный эквивалент (MSVC `/analyze` с `/WX`) выполнен и
  чист.
- **Точное следующее действие:** после push ветки убедиться, что задания
  `msvc-analyze` и `codeql` зелёные, и разобрать находки CodeQL, если они будут.

### B5 — автоматизация upstream не проверена сквозным запуском

- **Серьёзность:** низкая, не блокирующая.
- **Причина:** процесс срабатывает только при появлении выпуска новее 1.10.2;
  сейчас встроенная версия актуальна, поэтому ветка `validate`/`task` не
  исполнялась. Разбор дельты, контракт отчёта и политика процесса покрыты
  тестом без сети.
- **Точное следующее действие:** запустить процесс вручную
  (`workflow_dispatch`) после появления следующего выпуска Flowseal и проверить,
  что создаётся ровно одна задача и повторный запуск её обновляет, а не дублирует.

## Release decision

Все три P0-задачи закрыты и проверены: семантика версий RC/stable исправлена в
единственном источнике версии и подтверждена реальными метаданными выпуска;
нагрузка обновлена до Flowseal 1.10.2 штатным защищённым импортёром с
побайтовой сверкой; подпись Authenticode для стабильного тега сделана
обязательной, а опубликованные байты привязаны к подписанным по SHA-256 в трёх
местах.

Сверх этого закрыты пять из шести задач P1 и найдены три дефекта, которых в
задании не было: полный импорт upstream не мог завершиться ни для какой версии,
порог совместимости отсекал бы установленные RC при переходе на стабильный
выпуск, а ревизия upstream вида `1.10.2a` считалась понижением.

Стабильным выпуск назвать нельзя: два обязательных шлюза — проверка в чистом
окружении Windows 10 и Windows 11 и фактическая подпись сертификатом CHEBURNET —
не выполнены, и выполнить их в текущем окружении невозможно без изменения
рабочей машины владельца и без сертификата. Заявлять готовность при
непроверенных обязательных шлюзах было бы неверно.

Поэтому статус — `RELEASE_CANDIDATE_READY_WITH_BLOCKERS`: код и оснастка готовы,
осталось выполнить B1, B2 и B3 в указанном порядке.
