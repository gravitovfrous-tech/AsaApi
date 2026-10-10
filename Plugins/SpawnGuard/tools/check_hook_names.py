#!/usr/bin/env python3
"""Сверяет строки имён хуков в Main.cpp с заголовками AsaApi и с DECLARE_HOOK.

Две проверки:

1. каждая константа ``kHook*`` должна дословно совпадать со строкой из
   заголовка AsaApi (тот же класс, метод и список типов параметров);
2. список типов в константе должен соответствовать типы в соответствующем
   ``DECLARE_HOOK(...)`` из того же Main.cpp: константа - это ключ кэша, а
   DECLARE_HOOK - реальная сигнатура компилируемого хука. Если они разойдутся
   по размеру параметра, сервер упадёт; расхождение вроде TVector/TRotator
   (24 байта в обоих случаях) помечается как WARN.

Имя хука - это ключ, по которому AsaApi ищет оффсет в cached_offsets.cache, и
оно должно СЛОВО В СЛОВО совпадать со строкой в сгенерированном заголовке
(AsaApi/Core/Public/API/ARK/*.h). Одна опечатка в списке параметров - и хук не
встанет: плагин молча ничего не блокирует.

Запуск (из корня репозитория, рядом с папкой AsaApi):

    python Plugins/SpawnGuard/tools/check_hook_names.py

Код возврата 1, если имя в плагине не найдено ни в одном заголовке.
"""
import os
import re
import sys

PLUGIN_DIR = os.path.dirname(os.path.abspath(os.path.join(__file__, '..')))
ROOT = os.path.abspath(os.path.join(PLUGIN_DIR, '..', '..'))
HEADER_ROOTS = [
    os.path.join(ROOT, 'AsaApi', 'Core', 'Public', 'API'),
    os.path.join(ROOT, 'AsaApi', 'Core', 'Public'),
]

# Имена в Main.cpp: constexpr const char* kHookXxx = "Имя.Метод(...)";
CONST_RE = re.compile(r'constexpr\s+const\s+char\s*\*\s*(kHook\w+)\s*=\s*"([^"]+)"')
DECLARE_RE = re.compile(r'DECLARE_HOOK\s*\(')

# Пары типов, которые занимают одинаковый объём и передаются одинаково.
EQUIVALENT_PAIRS = [
    ('UE::Math::TVector<double>', 'UE::Math::TRotator<double>'),
    ('UE::Math::TVector<float>', 'UE::Math::TRotator<float>'),
    ('FVector', 'FRotator'),
]


def split_top_level(text, separator=','):
    """Делит строку по разделителю вне <>, () и []."""
    parts, current, depth = [], [], 0
    for ch in text:
        if ch in '<([':
            depth += 1
        elif ch in '>)]':
            depth -= 1
        if ch == separator and depth == 0:
            parts.append(''.join(current))
            current = []
            continue
        current.append(ch)
    parts.append(''.join(current))
    return [p.strip() for p in parts]


def split_signature(signature):
    """'Класс.Метод(тип1,тип2)' -> ('Класс.Метод', ['тип1', 'тип2'])."""
    open_paren = signature.find('(')
    if open_paren < 0:
        return signature, []
    inner = signature[open_paren + 1:signature.rfind(')')]
    return signature[:open_paren], ([] if not inner.strip() else split_top_level(inner))


def normalize_param(param):
    """Пробелы не важны, ссылка - тот же указатель, ведущий const не важен."""
    out = param.replace(' ', '').replace('\t', '').replace('&', '*')
    if out.startswith('const'):
        out = out[5:]
    return out


def params_compatible(expected, actual):
    """Совместимы ли типы (True), и есть ли текстовое различие (False/True)."""
    a, b = normalize_param(expected), normalize_param(actual)
    if a == b:
        return True, False
    for left, right in EQUIVALENT_PAIRS:
        if (a == normalize_param(left) and b == normalize_param(right)) or \
           (a == normalize_param(right) and b == normalize_param(left)):
            return True, True
    return False, True


def parse_declare_hooks(text):
    """Все DECLARE_HOOK(...) файла: имя -> (тип возврата, [типы параметров])."""
    hooks = {}
    for match in DECLARE_RE.finditer(text):
        start = match.end()
        depth, index = 1, start
        while index < len(text) and depth:
            if text[index] == '(':
                depth += 1
            elif text[index] == ')':
                depth -= 1
            index += 1
        parts = split_top_level(text[start:index - 1])
        if len(parts) >= 2:
            hooks[parts[0]] = (parts[1], parts[2:])
    return hooks
STRING_RE = re.compile(r'"([A-Za-z_][A-Za-z0-9_]*\.[A-Za-z0-9_]*\([^"]*\))"')


def collect_header_names():
    """Все строки вида "Класс.Метод(параметры)" из заголовков AsaApi."""
    found = {}
    for hroot in HEADER_ROOTS:
        if not os.path.isdir(hroot):
            continue
        for dirpath, _dirs, files in os.walk(hroot):
            for name in files:
                if not name.endswith('.h'):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    with open(path, encoding='utf-8', errors='ignore') as fh:
                        text = fh.read()
                except OSError:
                    continue
                for sig in STRING_RE.findall(text):
                    found.setdefault(sig, path)
    return found


def main():
    main_cpp = os.path.join(PLUGIN_DIR, 'Main.cpp')
    with open(main_cpp, encoding='utf-8') as fh:
        plugin_text = fh.read()

    constants = CONST_RE.findall(plugin_text)
    if not constants:
        print('FAIL: в Main.cpp не найдено ни одной константы kHook*')
        return 1

    header_names = collect_header_names()
    if not header_names:
        print('WARN: не найдено заголовков AsaApi (%s) - сверка пропущена'
              % ', '.join(HEADER_ROOTS))
        return 0

    hooks = parse_declare_hooks(plugin_text)
    problems = 0
    for name, value in constants:
        # --- проверка 2: константа против DECLARE_HOOK ---
        method = split_signature(value)[0]
        hook_name = method.replace('.', '_')
        if hook_name in hooks:
            # первый параметр DECLARE_HOOK - это неявный `this` (указатель на
            # объект класса), в строке-ключе кэша его нет
            declared_params = hooks[hook_name][1][1:]
            constant_params = split_signature(value)[1]
            if len(declared_params) != len(constant_params):
                problems += 1
                print('FAIL: %s: у DECLARE_HOOK(%s) %d параметр(ов), а в строке - %d'
                      % (name, hook_name, len(declared_params), len(constant_params)))
            else:
                soft = []
                hard = []
                for i, (declared, in_string) in enumerate(zip(declared_params, constant_params)):
                    compatible, differs = params_compatible(declared, in_string)
                    if not compatible:
                        hard.append('параметр %d: DECLARE_HOOK %r, строка %r'
                                    % (i + 1, declared, in_string))
                    elif differs:
                        soft.append('параметр %d: DECLARE_HOOK %r, строка %r'
                                    % (i + 1, declared, in_string))
                if hard:
                    problems += 1
                    print('FAIL: %s: строка не совпадает с DECLARE_HOOK(%s):' % (name, hook_name))
                    for line in hard:
                        print('      %s' % line)
                elif soft:
                    print('WARN: %s: строка и DECLARE_HOOK(%s) ABI-совместимы, но текст разный:'
                          % (name, hook_name))
                    for line in soft:
                        print('      %s' % line)
        else:
            print('WARN: %s: для %s не найден DECLARE_HOOK - проверьте имя вручную'
                  % (name, hook_name))

        # --- проверка 1: константа против заголовков SDK ---
        path = header_names.get(value)
        if path:
            print('OK  : %-34s %s' % (name, os.path.relpath(path, ROOT)))
            continue

        problems += 1
        print('FAIL: %s в Main.cpp не совпадает ни с одним заголовком AsaApi:' % name)
        print('      в плагине: %s' % value)
        prefix = value.split('(')[0] + '('
        near = sorted(sig for sig in header_names if sig.startswith(prefix))
        if near:
            print('      похожие строки в заголовках:')
            for sig in near[:8]:
                print('        %s' % sig)
        else:
            print('      строк с началом %r в заголовках нет' % prefix)

    if problems:
        print('\nИтог: %d расхождение(й). Имя хука - это ключ кэша: поправьте '
              'константу в Main.cpp, скопировав строку из заголовка целиком.' % problems)
        return 1

    print('\nИтог: все имена хуков совпадают с заголовками AsaApi.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
