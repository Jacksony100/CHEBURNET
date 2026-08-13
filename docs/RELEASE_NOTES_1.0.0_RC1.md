# CHEBURNET 1.0.0 RC1

Первый публичный release candidate CHEBURNET: один переносимый EXE для
управляемого запуска проверенного `winws`-движка на Windows 10/11 x64.

> **Статус RC.** Автоматические проверки пройдены, но интерактивный smoke-тест
> на чистых Windows 10 и 11 VM ещё остаётся обязательным gate перед stable.

## Главное

- embedded upstream Flowseal `zapret-discord-youtube` **1.10.1**;
- динамический каталог стратегий без хардкода их количества;
- защищённый versioned runtime и rollback на known-good версию;
- fail-closed ECDSA P-256 обновления launcher и payload;
- четыре режима GameFilter;
- локальные логи и диагностика без телеметрии;
- исправленный Unicode-вывод и строгая проверка ACL открытого log-файла.

## Что скачать

- `CHEBURNET.exe` — единственный файл, необходимый обычному пользователю;
- `CHEBURNET.exe.sha256` — контрольная сумма launcher;
- `cheburnet-payload-1.10.1.cbpkg` — подписываемый update payload;
- `update-manifest.json` + `.sig` — manifest и detached ECDSA-подпись;
- notices/licenses — исходные условия сторонних компонентов.

EXE в этом RC **не имеет Authenticode-подписи**. Это явно не скрывается:
проверяйте SHA-256 и скачивайте файлы только с этой страницы. Manifest
обновлений подписан отдельным встроенным ключом и проверяется fail-closed.

## Проверки перед публикацией

- чистая MSVC x64 Release-сборка: **PASS** (`/W4 /WX`);
- CTest: **33/33 PASS**;
- strategy fidelity: **84/84 PASS**;
- license validation: **PASS**;
- проверка подписи manifest публичным ключом: **PASS**;
- интерактивный Windows 10/11 VM smoke: **PENDING**.

Подробности — в
[CHANGELOG.md](https://github.com/Jacksony100/CHEBURNET/blob/v1.0.0-rc.1/CHANGELOG.md),
инструкция ручной проверки — в
[SMOKE_TEST.md](https://github.com/Jacksony100/CHEBURNET/blob/v1.0.0-rc.1/docs/SMOKE_TEST.md).
