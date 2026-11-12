
from krita import Krita, InfoObject
import os
import re
import traceback

# ============================================
# НАСТРОЙКИ
# ============================================

GROUP_NAME = "group"

EXCLUDED_LAYERS = {
    "frame_00",
    "frame_01",
    "glowing",
    "it_build_icon",
}

FILE_PREFIX = "it_trench_dirt_"
EXPORT_FOLDER = "tga_export"

# 70% в диапазоне Krita 0-255
OPACITY_70 = round(255 * 0.70)

# ============================================
# ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
# ============================================

def find_node(parent, name):
    for node in parent.childNodes():
        if node.name() == name:
            return node

        result = find_node(node, name)
        if result is not None:
            return result

    return None


def sanitize_name(name):
    name = name.strip().replace(" ", "_")
    return re.sub(r'[<>:"/\\|?*]', "_", name)


def get_all_nodes(parent):
    result = []

    for node in parent.childNodes():
        result.append(node)
        result.extend(get_all_nodes(node))

    return result


def isolate_group(group, root):
    """
    Оставляет видимой только выбранную группу
    и необходимые родительские группы.
    """
    current = group

    while current is not None and current != root:
        parent = current.parentNode()

        if parent is None:
            break

        for sibling in parent.childNodes():
            if sibling != current:
                sibling.setVisible(False)

        current.setVisible(True)
        current = parent


def export_tga(doc, path):
    # Синхронное обновление изображения
    doc.refreshProjection()

    options = InfoObject()

    success = doc.exportImage(path, options)

    if not success:
        raise RuntimeError(
            "Не удалось экспортировать: " + path
        )

    print("  OK:", os.path.basename(path))


# ============================================
# ОСНОВНАЯ ФУНКЦИЯ
# ============================================

def main():

    app = Krita.instance()
    doc = app.activeDocument()

    if doc is None:
        raise RuntimeError("Нет активного документа!")

    if not doc.fileName():
        raise RuntimeError(
            "Сначала сохрани документ в формате .kra!"
        )

    root = doc.rootNode()

    # ----------------------------------------
    # Поиск группы и служебных слоёв
    # ----------------------------------------

    group = find_node(root, GROUP_NAME)

    if group is None:
        raise RuntimeError(
            'Группа "' + GROUP_NAME + '" не найдена!'
        )

    children = group.childNodes()

    def get_child(name):
        for node in children:
            if node.name() == name:
                return node
        raise RuntimeError(
            'Не найден слой "' + name + '"'
        )

    frame_00 = get_child("frame_00")
    frame_01 = get_child("frame_01")
    glowing = get_child("glowing")

    # ----------------------------------------
    # Список слоёв для экспорта
    # ----------------------------------------

    supported_types = {
        "paintlayer",
        "vectorlayer",
        "filelayer",
        "grouplayer",
        "clonelayer",
        "filllayer",
        "filterlayer",
    }

    layers = [
        node
        for node in children
        if node.name() not in EXCLUDED_LAYERS
        and node.type() in supported_types
    ]

    if not layers:
        raise RuntimeError(
            "Не найдено слоёв для экспорта!"
        )

    # Проверка уникальности имён файлов
    names = [
        sanitize_name(node.name()).lower()
        for node in layers
    ]

    if len(names) != len(set(names)):
        raise RuntimeError(
            "Обнаружены повторяющиеся имена слоёв!"
        )

    # ----------------------------------------
    # Папка для экспорта
    # ----------------------------------------

    base_dir = os.path.dirname(doc.fileName())

    export_dir = os.path.join(
        base_dir,
        EXPORT_FOLDER
    )

    os.makedirs(export_dir, exist_ok=True)

    # ----------------------------------------
    # Сохранение исходного состояния
    # ----------------------------------------

    all_nodes = get_all_nodes(root)

    original_visibility = [
        (node, node.visible())
        for node in all_nodes
    ]

    original_opacity = group.opacity()
    original_batchmode = doc.batchmode()
    original_active_node = doc.activeNode()

    # ----------------------------------------
    # Конфигурации экспорта
    # ----------------------------------------

    combinations = [
        # Номер, рамка, свечение, непрозрачность

        ("00", frame_00, False, 255),
        ("01", frame_01, False, 255),
        ("02", frame_01, True,  255),
        ("03", frame_01, False, OPACITY_70),
    ]

    total = len(layers) * len(combinations)
    exported = 0

    print("=" * 50)
    print("НАЧАЛО ЭКСПОРТА")
    print("Слоёв:", len(layers))
    print("Файлов:", total)
    print("Папка:", export_dir)
    print("=" * 50)

    # ----------------------------------------
    # Экспорт с гарантированным восстановлением
    # ----------------------------------------

    try:
        doc.setBatchmode(True)

        # Скрываем другие группы и слои документа
        isolate_group(group, root)

        group.setVisible(True)

        for layer_index, layer in enumerate(layers, 1):

            print()
            print(
                "[{}/{}] {}".format(
                    layer_index,
                    len(layers),
                    layer.name()
                )
            )

            safe_name = sanitize_name(layer.name())

            for suffix, frame, use_glowing, opacity in combinations:

                # Скрываем все основные слои группы
                for node in children:
                    if node.type() in supported_types:
                        node.setVisible(False)

                # Настраиваем композицию
                group.setOpacity(opacity)

                frame.setVisible(True)
                layer.setVisible(True)

                if use_glowing:
                    glowing.setVisible(True)

                # Имя итогового файла
                filename = (
                    FILE_PREFIX
                    + safe_name
                    + "_"
                    + suffix
                    + ".tga"
                )

                path = os.path.join(
                    export_dir,
                    filename
                )

                # Экспорт
                export_tga(doc, path)

                exported += 1

        print()
        print("=" * 50)
        print("ЭКСПОРТ ЗАВЕРШЁН")
        print("Экспортировано:", exported)
        print("Папка:", export_dir)
        print("=" * 50)

    finally:
        # ------------------------------------
        # Восстановление исходного состояния
        # ------------------------------------

        print()
        print("Восстановление состояния документа...")

        for node, visible in original_visibility:
            node.setVisible(visible)

        group.setOpacity(original_opacity)

        doc.setBatchmode(original_batchmode)

        if original_active_node is not None:
            doc.setActiveNode(original_active_node)

        doc.refreshProjection()

        print("Состояние слоёв восстановлено.")


# ============================================
# ЗАПУСК
# ============================================

try:
    main()
except Exception:
    print()
    print("ОШИБКА:")
    traceback.print_exc()
