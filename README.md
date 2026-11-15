# Immersive Trenches

Мод для **Call to Arms – Gates of Hell: Ostfront** добавляет инженерам меню врзможность строительства разных элемонтов окопов. Имеются прямые секции, повороты, развилки, выходы, земляные доты, огневые позиции и окопное депо, которое пополняет боеприпасы поблизости.

При размещении секции прилипают к свободным стыкам и разворачиваются вдоль них. Несколько секций можно поставить в очередь строительства. Свободные торцы готовых окопов закрываются заглушками, которые снимаются при присоединении новой секции.

## Компиляция и установка

Для сборки нужны **Visual Studio 2026** или **Build Tools 2026** с MSVC v145 и Windows SDK. Закройте игру, GEM Editor и локальный сервер, затем откройте PowerShell в корне проекта и выполните:

```powershell
.\Install.ps1 -GameRoot 'C:\Program Files (x86)\Steam\steamapps\common\Call to Arms - Gates of Hell'
```

Если PowerShell блокирует скрипт, запустите `powershell -ExecutionPolicy Bypass -File .\Install.ps1`.

## Установка готовой сборки

Скачайте архив [из раздела Releases](https://github.com/SirWirtFrog/GoH-Immersive-trenches/releases) и скопируйте папки `binaries` и `mods` из архива в корень игры.

После установки запустите игру, активируйте **Immersive Trenches** в списке модов и выберите инженера. На панели команд под кнопкой **«Развернуть»** откройте **«Окопы»**.

Целевая версия игры и редактора — **1.067.0 x64**, сборка `aaa36ed4d`. Для сетевой игры всем участникам нужна одна версия мода.

## Дополнительные команды

Команды запускаются из корня проекта. Перед переустановкой или удалением закройте игру, редактор и локальный сервер.

| Задача | Команда |
| --- | --- |
| Собрать комплект для ручной установки | `.\scripts\BuildRelease.ps1` |
| Собрать и установить Debug | `.\Install.ps1 -Configuration Debug` |
| Переустановить мод начисто | `.\scripts\Reinstall.ps1` |
| Посмотреть список файлов для удаления | `.\scripts\uninstall.ps1 -WhatIf` |
| Удалить мод | `.\scripts\uninstall.ps1` |

Комплект для ручной установки сохраняется в `out\releases\ImmersiveTrenches-<дата-время>`. Для своего пути используйте `-OutputDirectory`. Переустановка и удаление убирают всю установленную папку мода, включая добавленные туда вручную файлы.

<details>
<summary>Технические сведения и проверки для разработчиков</summary>

Загрузчик `ImmersiveTrenchesMod.asi` загружается через Miles Sound System из `binaries\x64\mss`. При `RIB_Main(upDown=1)` он открывает `mods\ImmersiveTrenches\ImmersiveTrenchesModLogic.dll` и вызывает `GoHModInitialize` вне `DllMain`. Карта функций и структур движка находится в [GEM_FUNCTIONS.md](GEM_FUNCTIONS.md).

Сборки лежат в `out\x64\<Configuration>`. Для отладки установите `Debug|x64` и запустите GEM Editor через F5 из Visual Studio. Переменная `GoHGameRoot` задаёт нестандартный путь игры для профиля F5 и скриптов установки.

`DeployResources` архивирует распознанные устаревшие каталоги в `%LOCALAPPDATA%\ImmersiveTrenches\resource-backups`. Чтобы посмотреть план копирования без изменения файлов, передайте `-PlanOnly` скрипту `scripts\DeployResources.ps1` вместе с `-SourceRoot` и `-DestinationRoot`.
