#!/usr/bin/env bash
# Прогон тестов логики SpawnGuard без MSVC и без Windows.
#
#   bash Plugins/SpawnGuard/tools/tests/run_tests.sh
#
# Скрипт вырезает нужные блоки прямо из Main.cpp (поэтому тесты не разъедутся
# с исходником), подставляет их в шаблоны *.cpp.in вместе с заглушками Win32 и
# собирает всё компилятором C++14 - тем же стандартом, что используется в
# проекте Visual Studio.
#
# Требуется: python3 и g++ (подойдёт WSL или любая Linux-машина).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
main_cpp="$here/../../Main.cpp"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if ! command -v python3 >/dev/null 2>&1; then
    echo "нужен python3" >&2
    exit 2
fi
if ! command -v g++ >/dev/null 2>&1; then
    echo "нужен g++ (например, в WSL: sudo apt install g++)" >&2
    exit 2
fi

python3 - "$main_cpp" "$here" "$work" <<'PYEOF' || exit 2
import sys

main_cpp, here, work = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(main_cpp, encoding='utf-8').read()

# Блок A: разбор путей/кэша и сравнение подписей.
# Блок B: то же плюс struct HookName и ResolveHookName.
start = src.index('\tstd::string ToNarrowString(const std::wstring& text)')
end_signatures = src.index('\tstruct HookName')
end_resolve = src.index('\tvoid LogHookNameProblem(const char* what, const std::string& expected, const HookName& info')

blocks = {
    'test_signatures.cpp': src[start:end_signatures],
    'test_resolve.cpp': src[start:end_resolve],
}

for target, block in blocks.items():
    template = open('%s/%s.in' % (here, target), encoding='utf-8').read()
    marker = '// @PLUGIN_CODE@'
    assert marker in template, target
    open('%s/%s' % (work, target), 'w', encoding='utf-8').write(
        template.replace(marker, block, 1))
print('блоки Main.cpp извлечены')
PYEOF

cp "$here/stubs_win32.hpp" "$work/"

status=0
for test in test_signatures test_resolve; do
    echo "=== $test ==="
    if ! g++ -std=c++14 -Wall -Wextra -I"$work" -o "$work/$test" "$work/$test.cpp"; then
        echo "СБОРКА ТЕСТА ПРОВАЛЕНА: $test" >&2
        status=1
        continue
    fi
    ( cd "$work" && "./$test" ) || status=1
done

if [ "$status" -eq 0 ]; then
    echo "ИТОГ: все тесты пройдены"
else
    echo "ИТОГ: есть проваленные тесты" >&2
fi
exit "$status"
