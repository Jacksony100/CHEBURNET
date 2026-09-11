# Импорт Flowseal 1.10.1 → 1.10.2

Документ фиксирует результат Phase 2: перевод встроенной полезной нагрузки
CHEBURNET с Flowseal `1.10.1` на стабильный `1.10.2`.

## Способ импорта

Использован штатный защищённый импортёр, ручное копирование файлов не
применялось:

```powershell
scripts\sync-upstream.ps1 -Version 1.10.2
```

Результат:

```
UPSTREAM_IMPORT: PASS version=1.10.2 sha256=5eaac9fb2e4b1abd693487452a3ff3f4dfe9578a45f9ddddfa4bc1f5a6bb62d5
```

Импортёр отработал полный транзакционный сценарий: проверка неизменяемости
выпуска → сверка SHA-256 архива с digest релизного ассета → валидация каждой
записи ZIP до материализации → изолированная генерация стратегий и ресурсов →
чистая сборка и полный набор тестов в отдельном дереве → замена production-файлов
→ побайтовая сверка установленной нагрузки с архивом.

## Найденный и устранённый дефект импортёра

Первый запуск полного импорта завершился ошибкой **не из-за 1.10.2**, а из-за
самого импортёра:

```
32/34 Test #32: license_validation ...............***Failed
missing license: SECURITY.md
```

`sync-upstream.ps1` собирал изолированное дерево проверки из явного списка
файлов, в котором не было `SECURITY.md` и каталога `docs`. При этом
`scripts/validate-licenses.ps1` требует `SECURITY.md`, `docs/PRIVACY.md` и
`docs/UPDATE_SECURITY.md`. То есть полный импорт **не мог завершиться успешно
ни для какой версии upstream** — шлюз падал на отсутствующем файле репозитория,
а не на импортируемой нагрузке. Дефект существовал до текущих работ.

Исправление:

1. список копирования дополнен `docs`, `SECURITY.md`, `CHANGELOG.md`,
   `DEPENDENCIES.md`, `CONTRIBUTING.md`;
2. добавлен регрессионный тест `tests/importer_validation_tree.ps1`
   (тест CTest `importer_validation_tree`), который извлекает список
   копирования **из исходника самого импортёра**, собирает по нему дерево и
   прогоняет против него релизные шлюзы. Тест не требует сети и не скачивает
   нагрузку, поэтому расхождение списка и шлюзов больше не может пройти молча.

Проверено, что тест ловит исходный дефект: со старым списком
`validate-licenses.ps1` завершается с `missing license: SECURITY.md`, с новым —
`LICENSE_VALIDATION: PASS`.

## Provenance

`resources/upstream/provenance.json`:

| Поле | Было (1.10.1) | Стало (1.10.2) |
|---|---|---|
| version / tag | `1.10.1` | `1.10.2` |
| release_id | `367512178` | `375784527` |
| archive_sha256 | `f748d61f…76c9` | `5eaac9fb2e4b1abd693487452a3ff3f4dfe9578a45f9ddddfa4bc1f5a6bb62d5` |
| upstream_commit | `47da17f8…b938` | `dfd8e613b099676cf2aa7b474ee5923801514dec` |
| immutable | `true` | `true` |
| source_url | …/1.10.1/…zip | `https://github.com/Flowseal/zapret-discord-youtube/releases/download/1.10.2/zapret-discord-youtube-1.10.2.zip` |

`imported_at_utc`: `2026-09-11T07:13:00Z`.

## Состав изменений нагрузки

### Стратегии

| Изменение | Файл |
|---|---|
| добавлена | `general (ALT13).bat` |
| изменена | `general (EXP).bat` |

Количество стратегий: **21 → 22**. Идентификаторы типизированного каталога:

```
general, alt, alt2 … alt12, alt13, exp,
faketls_auto, faketls_auto_alt, faketls_auto_alt2, faketls_auto_alt3,
simple_fake, simple_fake_alt, simple_fake_alt2
```

Счётчик стратегий в рантайме остаётся динамическим — это отдельно проверяется
регрессией `STRATEGY runtime has no fixed strategy count`.

### Бинарные ресурсы

| Изменение | Файл |
|---|---|
| добавлен | `bin/tls_clienthello_sochi_park.bin` |
| переименован | `bin/quic_initial_4pda.to.bin` → `bin/quic_initial_4pda_to.bin` |
| удалён | `bin/quic_initial_dbankcloud_ru.bin` |

Проверено, что удалённые и переименованные ресурсы **не остались** во встроенной
нагрузке: после чистой переконфигурации ни `quic_initial_4pda.to.bin`, ни
`quic_initial_dbankcloud_ru.bin` не присутствуют в `GeneratedManifest.h` и
`cheburnet_payload.rc` (проверка по точному совпадению строки). Новые ресурсы
присутствуют. Это ожидаемо: `resources/payload` заменяется целиком, а
`.rc`-манифест генерируется из каталога нагрузки на этапе configure.

### Списки

| Файл | Изменение |
|---|---|
| `lists/list-general.txt` | +9 строк |
| `lists/list-exclude.txt` | +27 / −6 строк |

## Совместимость импортёра

Расширение грамматики строгого парсера **не потребовалось**. `ALT13` использует
уже поддерживаемый синтаксис, включая `--dpi-desync=fake,hostfakesplit`,
`--dpi-desync-hostfakesplit-mod=host=…,altorder=1` и `--dpi-desync-fake-unknown`.
Изменение в `general (EXP).bat` — добавление `--dpi-desync-cutoff=n4` и
переименование ссылки на `quic_initial_4pda_to.bin`.

Неизвестный синтаксис по-прежнему отклоняется fail-closed; BAT-файлы upstream
в обычном рантайме не исполняются.

## Проверка

Все команды выполнены на ветке `release/production-hardening-v1`.

| Проверка | Команда | Результат |
|---|---|---|
| Чистая сборка | `scripts\build-release.ps1 -BuildDir build-p2` | **PASS**, `CHEBURNET.exe` 3.92 MB, предупреждений нет (`/W4 /WX`) |
| Пакет обновления | (в составе сборки) | `UPDATE_PACKAGE: PASS version=1.10.2 files=27 bytes=3403061 sha256=970c8550…3333` |
| Полный набор тестов | `scripts\run-tests.ps1 -BuildDir build-p2` | **35/35 PASS** |
| Достоверность стратегий | (в составе `run-tests.ps1`) | `STRATEGY_FIDELITY: 88/88 PASS (22 strategies x 4 modes)` |
| Достоверность upstream | `scripts\verify-upstream.ps1` | `UPSTREAM_FIDELITY: PASS version=1.10.2 files=46 strategies=22 archive_sha256=5eaac9fb…62d5` |
| Идемпотентность импорта | `scripts\sync-upstream.ps1 -Version 1.10.2 -CheckOnly` | `UPSTREAM_IMPORT: already current (1.10.2)` |
| Дерево проверки импортёра | тест CTest `importer_validation_tree` | **PASS** (15 записей, релизные шлюзы удовлетворены) |

Режимы GameFilter покрыты полностью: 22 стратегии × 4 режима
(`Off`, `All`, `Tcp`, `Udp`) = 88 независимых сверок аргументов `winws`
с независимым разбором исходных BAT-файлов.

## Риски

- Поведение новой стратегии `ALT13` в реальной сети не проверялось: CHEBURNET
  гарантирует лишь побайтовое соответствие импортированных аргументов
  upstream-оригиналу. Функциональная эффективность стратегии — зона
  ответственности upstream.
- Пользовательская конфигурация с сохранённым `strategyId`, отсутствующим в
  1.10.2, будет сброшена на `general` с записью в журнал. В 1.10.2 ни одна
  стратегия не удалена, поэтому практического влияния нет.
- Размер пакета обновления вырос незначительно (3 399 786 → 3 403 061 байт).
