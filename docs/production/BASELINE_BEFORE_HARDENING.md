# CHEBURNET — базовое состояние перед production-hardening

Документ фиксирует воспроизводимое состояние репозитория **до** каких-либо
изменений в рамках подготовки стабильного выпуска `v1.0.0`.

## Git

| Параметр | Значение |
|---|---|
| Базовая ветка | `main` |
| Базовый commit (HEAD до изменений) | `78cc980a6d0d68f669cf98eff3799c1cf5fa02bd` |
| Тег на базовом commit | `v1.0.0-rc.3` |
| Рабочее дерево | чистое (`git status --porcelain` — пусто) |
| Рабочая ветка изменений | `claude/production-hardening-v1` |
| Существующие теги | `v1.0.0-rc.1`, `v1.0.0-rc.2`, `v1.0.0-rc.3` |

## Окружение сборки

| Параметр | Значение |
|---|---|
| ОС | Windows 11 Pro 10.0.26200 |
| Тулчейн | MSVC (Visual Studio 18 BuildTools), x64 |
| Генератор | Ninja |
| Конфигурация | Release |
| Стандарт | C++20, `/W4 /WX /permissive- /utf-8`, `/guard:cf`, статический CRT (`/MT`) |

## Результат чистой сборки

```
scripts\build-release.ps1 -BuildDir build-p0-baseline
```

- Результат: **PASS**
- Артефакт: `build-p0-baseline\CHEBURNET.exe`, 3.91 MB
- Предупреждения компилятора: **нет** (сборка идёт с `/WX`, любое предупреждение было бы ошибкой)
- Встроенный тестовый пакет обновления: `UPDATE_PACKAGE: PASS version=1.10.1 files=27 bytes=3399786 sha256=41004f8523af4c176d859590ee8b617834df048e892006dab8cc77cfbd6b7319`

## Результат полного набора тестов

```
scripts\run-tests.ps1 -BuildDir build-p0-baseline
```

- CTest: **33/33 PASS**, 0 падений, общее время 7.04 s
- Независимая кросс-проверка стратегий: `STRATEGY_FIDELITY: 84/84 PASS (21 strategies x 4 modes)`
- Итог: `ALL TESTS PASSED`

Состав CTest (33 теста):

| Категория | Тесты |
|---|---|
| Аргументы/строки | `args`, `quoting` |
| Конфигурация | `config`, `configmigration` |
| Криптография/целостность | `sha256`, `integrity` |
| Манифест ресурсов | `manifest` |
| Процессы | `process`, `preflight`, `operationstate` |
| Файловая система | `securefs`, `loghandle` |
| UI | `animation`, `progress`, `mascot`, `layout`, `checkpoints`, `menu`, `effects`, `theme`, `framebuffer` |
| JSON | `jsonstrict` |
| Обновления | `updatemanifest`, `updateversion`, `updatesignature`, `updatepackage`, `updaterollback`, `updatestate`, `updatehttp` |
| Скриптовые шлюзы | `package_safety`, `security_regression`, `license_validation`, `release_metadata` |

## Текущая версия upstream

`resources/upstream/provenance.json`:

| Поле | Значение |
|---|---|
| provider | `Flowseal/zapret-discord-youtube` |
| version / tag | `1.10.1` |
| release_id | `367512178` |
| archive_sha256 | `f748d61fec75e4edc992cb5b09d554e914197c68c690384aceb61f143d8f76c9` |
| upstream_commit | `47da17f80ad36a8424cdd25658153fdebd7eb938` |
| immutable | `true` |

Встроенных стратегий: **21** (`general` + `general (ALT)` … `general (ALT12)` + `general (EXP)` и варианты), проверено в 4 режимах GameFilter.

Доступная стабильная версия upstream на момент фиксации baseline: **1.10.2**
(`zapret-discord-youtube-1.10.2.zip`, `sha256:5eaac9fb2e4b1abd693487452a3ff3f4dfe9578a45f9ddddfa4bc1f5a6bb62d5`,
`immutable=true`, `prerelease=false`).

## Текущее поведение семантической версии лончера (дефект)

Источник версии — единственный: `project(CHEBURNET VERSION 1.0.0)` в `CMakeLists.txt`.

`cmake/GeneratedVersion.h.in` раскрывается в:

```c
#define CHEBURNET_VERSION_QUAD   1,0,0,0
#define CHEBURNET_VERSION_STR    "1.0.0"
#define CHEBURNET_VERSION_RC_STR "1.0.0.0"   // значение PE FileVersion/ProductVersion
#define CHEBURNET_VERSION_WSTR   L"1.0.0"
```

Следствия на baseline:

1. Бинарник, собранный для тега `v1.0.0-rc.3`, во время выполнения
   идентифицирует себя как `1.0.0` (`CHEBURNET_VERSION_STR`).
2. `UpdateManager::CheckNow` сравнивает именно `CHEBURNET_VERSION_STR`
   с версией из подписанного манифеста
   (`src/update/UpdateManager.cpp:138`, `src/update/UpdateManager.cpp:150`).
3. `scripts/prepare-release.ps1` передаёт в генератор манифеста
   `-LauncherVersion $sourceVersion`, то есть `1.0.0`, **и для RC-тегов тоже**.
4. Поэтому будущий стабильный `1.0.0` оценивается установленным
   `v1.0.0-rc.3` как `Eligibility::Current`, а не `Upgrade`.

При этом сам компаратор версий корректно упорядочивает prerelease:
`CompareVersions("1.0.0-rc.3", "1.0.0") < 0`. Дефект — исключительно в том,
что prerelease-метка вообще не попадает в версию времени выполнения и в
метаданные выпуска. Это и есть предмет Phase 1; компаратор ослаблять не требуется.

Дополнительно зафиксированы связанные ограничения baseline:

- `scripts/generate-update-manifest.ps1` требует строгого равенства
  `FileVersion == "<LauncherVersion>.0"`, то есть семантическая версия и
  числовая PE-версия жёстко склеены и не могут различаться.
- `scripts/prepare-release.ps1` допускает RC-тег при стабильной исходной
  версии, но не умеет выразить RC в метаданных выпуска.

## Текущий release workflow (ключевые наблюдения)

`.github/workflows/release.yml`:

- шаг подписи называется «Необязательная подпись и проверка Authenticode» и
  вызывает `scripts/authenticode-sign.ps1`, который при отсутствии
  `AUTHENTICODE_PFX_B64` печатает предупреждение и завершается с кодом `0`;
- то есть стабильный тег `vX.Y.Z` на baseline **может быть опубликован без подписи**;
- порядок «подпись → упаковка → хеш» сам по себе корректен: подпись ставится
  на `build-release\CHEBURNET.exe`, а `package.ps1` копирует этот файл в `dist`
  и только затем считает SHA-256 — то есть хеш уже считается с подписанного файла;
- `permissions: contents: write` — минимальные права для публикации релиза;
- сторонние actions закреплены по неизменяемым commit SHA.

`.github/workflows/upstream-check.yml` выполняет проверку новых выпусков
upstream, но не создаёт устойчивую задачу (PR/issue) — предмет Phase 4.

## Текущие метаданные RC-выпуска

Для тега `v1.0.0-rc.3` публикуются:

- `CHEBURNET.exe` + `CHEBURNET.exe.sha256`
- `cheburnet-payload-1.10.1.cbpkg` + `.sha256`
- `update-manifest.json` + `update-manifest.json.sig`
- `THIRD_PARTY_NOTICES.md`, `DEPENDENCIES.md`, `LICENSES/*`

`prerelease` в GitHub Release выставляется по наличию `-rc.` в имени тега.
Внутри `update-manifest.json` канал всегда `stable`, а `launcher.version` на
baseline равен `1.0.0` даже для RC — см. дефект выше.

## Известные предупреждения / шум

- Предупреждений компилятора нет.
- В корне рабочей копии присутствует множество локальных каталогов сборки
  (`build*`, `dist`, `artifacts`) — все они в `.gitignore` и на состояние
  репозитория не влияют.

## Изменения поведения в этой фазе

Отсутствуют. Phase 0 — только фиксация состояния.
